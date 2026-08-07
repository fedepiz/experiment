
#include "base/core.h"
#include "base/math.h"
#include "base/arena.h"
#include "base/tctx.h"
#include "base/strings.h"
#include "gfx/window.h"

// The OS_WINDOWS guard keeps the file quiet in clangd, which parses it
// standalone on every platform (no windows.h off Windows); real builds only
// include it on Windows anyway, via gfx/window.c.
#if OS_WINDOWS

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

////////////////////////////////
//~ fp: Win32 Backend
//
// One window, message-pump driven. The WndProc can only hand events somewhere
// while wnd_get_events is pumping, so the state carries the pump's arena and
// list for exactly that duration; messages arriving outside a pump (during
// CreateWindowEx, mostly) fall through to DefWindowProc untranslated.
//
// The Win32 API speaks physical pixels. The process opts into per-monitor DPI
// awareness (otherwise Windows lies about sizes and bitmap-stretches the
// swapchain), and everything public converts through wnd_scale -- pixels per
// point, 1.5 at 150% -- so the API speaks points like the mac backend.

typedef struct {
  HWND hwnd;
  HDC hdc;     // CS_OWNDC: private, stable for the GL context's lifetime
  HGLRC hglrc;
  Arena* evt_arena;        // non-zero only while wnd_get_events pumps
  WND_EventList* evt_list;
  HMONITOR refresh_monitor; // cache key for refresh_hz: re-query on change
  F32 refresh_hz;
  void* swap_interval_proc; // PFN_wglSwapIntervalEXT, kept from equip; 0 if absent
  B32 is_open;
} WND_State;

global WND_State wnd_state;

// same job as os_win__wide_from_str8, redone locally: the os backend's helpers
// are its own, and the layers stay independent
internal WCHAR* wnd__wide_from_str8(Arena* arena, String8 s) {
  int count = 0;
  if(s.size > 0) {
    count = MultiByteToWideChar(CP_UTF8, 0, (char*)s.str, (int)s.size, 0, 0);
  }
  WCHAR* result = push_array_no_zero(arena, WCHAR, (U64)count + 1);
  if(count > 0) {
    MultiByteToWideChar(CP_UTF8, 0, (char*)s.str, (int)s.size, result, count);
  }
  result[count] = 0;
  return result;
}

////////////////////////////////
//~ fp: Event Translation

internal WND_Key wnd__key_from_vk(U32 vk) {
  WND_Key result = WND_Key_Nil;
  if('A' <= vk && vk <= 'Z') { result = (WND_Key)(WND_Key_A + (vk - 'A')); }
  else if('0' <= vk && vk <= '9') { result = (WND_Key)(WND_Key_0 + (vk - '0')); }
  else if(VK_F1 <= vk && vk <= VK_F12) { result = (WND_Key)(WND_Key_F1 + (vk - VK_F1)); }
  else {
    switch(vk) {
      case VK_LEFT:      result = WND_Key_Left; break;
      case VK_RIGHT:     result = WND_Key_Right; break;
      case VK_UP:        result = WND_Key_Up; break;
      case VK_DOWN:      result = WND_Key_Down; break;
      case VK_ESCAPE:    result = WND_Key_Escape; break;
      case VK_SPACE:     result = WND_Key_Space; break;
      case VK_RETURN:    result = WND_Key_Enter; break;
      case VK_TAB:       result = WND_Key_Tab; break;
      case VK_BACK:      result = WND_Key_Backspace; break;
      case VK_DELETE:    result = WND_Key_Delete; break;
      case VK_INSERT:    result = WND_Key_Insert; break;
      case VK_HOME:      result = WND_Key_Home; break;
      case VK_END:       result = WND_Key_End; break;
      case VK_PRIOR:     result = WND_Key_PageUp; break;
      case VK_NEXT:      result = WND_Key_PageDown; break;

      //- modifiers: plain WM_KEYDOWN wparams are already the side-agnostic
      //  VK_SHIFT/VK_CONTROL/VK_MENU, so the left/right collapse of the other
      //  backends comes for free
      case VK_SHIFT:     result = WND_Key_Shift; break;
      case VK_CONTROL:   result = WND_Key_Ctrl; break;
      case VK_MENU:      result = WND_Key_Alt; break;

      //- punctuation (VK_OEM_* positions on a US layout, like the kVK table)
      case VK_OEM_MINUS:  result = WND_Key_Minus; break;
      case VK_OEM_PLUS:   result = WND_Key_Equals; break;
      case VK_OEM_COMMA:  result = WND_Key_Comma; break;
      case VK_OEM_PERIOD: result = WND_Key_Period; break;
      case VK_OEM_2:      result = WND_Key_Slash; break;
      case VK_OEM_3:      result = WND_Key_Backtick; break;
    }
  }
  return result;
}

internal WND_Modifiers wnd__modifiers_now(void) {
  // GetKeyState reads the state as of the message being processed -- exactly
  // "held at event time", matching the other backends
  WND_Modifiers result = 0;
  if(GetKeyState(VK_SHIFT) & 0x8000)   { result |= WND_Modifier_Shift; }
  if(GetKeyState(VK_CONTROL) & 0x8000) { result |= WND_Modifier_Ctrl; }
  if(GetKeyState(VK_MENU) & 0x8000)    { result |= WND_Modifier_Alt; }
  return result;
}

internal LRESULT CALLBACK wnd__window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  Arena* arena = wnd_state.evt_arena;
  WND_EventList* list = wnd_state.evt_list;
  if(arena == 0) { return DefWindowProcW(hwnd, msg, wparam, lparam); }

  LRESULT result = 0;
  F32 scale = wnd_scale();
  switch(msg) {
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP: {
      WND_Key key = wnd__key_from_vk((U32)wparam);
      B32 is_down = (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN);
      // lparam bit 30 set on a down means the key was already down: an
      // autorepeat, not a transition -- KeyDown events mean transitions
      B32 is_repeat = is_down && (lparam & (1 << 30));
      if(key != WND_Key_Nil && !is_repeat) {
        WND_Event* event = wnd__push_event(arena, list, is_down ?
                                           WND_EventType_KeyDown : WND_EventType_KeyUp);
        event->key = key;
        event->modifiers = wnd__modifiers_now();
      }
      // syskeys (Alt-anything, F10) normally go to DefWindowProc, whose menu
      // loop eats the next keypress on a bare Alt tap; there is no menu, so
      // swallow them all except Alt+F4, which DefWindowProc turns into WM_CLOSE
      if((msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP) && wparam == VK_F4) {
        result = DefWindowProcW(hwnd, msg, wparam, lparam);
      }
    } break;

    case WM_LBUTTONDOWN: case WM_LBUTTONUP:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: {
      WND_MouseButton button = (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP) ? WND_MouseButton_Left :
                               (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP) ? WND_MouseButton_Right :
                                                                                WND_MouseButton_Middle;
      B32 is_down = (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN);
      // capture while any button is held so an up after dragging out of the
      // window still arrives (X11 grabs implicitly, mac filters in cocoa)
      if(is_down) { SetCapture(hwnd); }
      else if((wparam & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON)) == 0) { ReleaseCapture(); }
      WND_Event* event = wnd__push_event(arena, list, is_down ?
                                         WND_EventType_MouseDown : WND_EventType_MouseUp);
      event->button = button;
      event->pos.x = (F32)(I32)(I16)LOWORD(lparam) / scale;
      event->pos.y = (F32)(I32)(I16)HIWORD(lparam) / scale;
      event->modifiers = wnd__modifiers_now();
    } break;

    case WM_MOUSEMOVE: {
      WND_Event* event = wnd__push_event(arena, list, WND_EventType_MouseMoved);
      event->pos.x = (F32)(I32)(I16)LOWORD(lparam) / scale;
      event->pos.y = (F32)(I32)(I16)HIWORD(lparam) / scale;
      event->modifiers = wnd__modifiers_now();
    } break;

    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL: {
      // wheel positions are screen-relative, unlike every other mouse message
      POINT p = {(I32)(I16)LOWORD(lparam), (I32)(I16)HIWORD(lparam)};
      ScreenToClient(hwnd, &p);
      // one detent is WHEEL_DELTA; forward (away from the user) is positive,
      // matching the +y convention, and right is +x
      F32 steps = (F32)GET_WHEEL_DELTA_WPARAM(wparam) / (F32)WHEEL_DELTA;
      WND_Event* event = wnd__push_event(arena, list, WND_EventType_Scroll);
      if(msg == WM_MOUSEWHEEL) { event->scroll.y = steps; }
      else                     { event->scroll.x = steps; }
      event->pos.x = (F32)p.x / scale;
      event->pos.y = (F32)p.y / scale;
      event->modifiers = wnd__modifiers_now();
    } break;

    case WM_SIZE: {
      // fires only on actual size changes (moves come as WM_MOVE), so no
      // dedup is needed; skip the minimize's 0x0 client, which is not a size
      // anyone wants to lay out against
      if(wparam != SIZE_MINIMIZED) {
        WND_Event* event = wnd__push_event(arena, list, WND_EventType_Resize);
        event->size.x = (F32)LOWORD(lparam) / scale;
        event->size.y = (F32)HIWORD(lparam) / scale;
      }
    } break;

    case WM_CLOSE: {
      // don't DestroyWindow here: report, and let the app decide (it calls
      // wnd_close on its way out)
      wnd__push_event(arena, list, WND_EventType_CloseRequested);
    } break;

    default: {
      result = DefWindowProcW(hwnd, msg, wparam, lparam);
    } break;
  }
  return result;
}

internal WND_EventList wnd_get_events(Arena* arena) {
  WND_EventList list = {0};
  if(!wnd_state.is_open) { return list; }
  wnd_state.evt_arena = arena;
  wnd_state.evt_list = &list;
  for(MSG msg; PeekMessageW(&msg, 0, 0, 0, PM_REMOVE);) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  wnd_state.evt_arena = 0;
  wnd_state.evt_list = 0;
  return list;
}

////////////////////////////////
//~ fp: Window

internal void wnd_open(String8 title, I32 w, I32 h) {
  // before any window exists, so the first one is already born DPI-aware
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

  HINSTANCE instance = GetModuleHandleW(0);
  WNDCLASSW wc = {0};
  wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
  wc.lpfnWndProc = wnd__window_proc;
  wc.hInstance = instance;
  wc.hCursor = LoadCursorW(0, (LPCWSTR)IDC_ARROW);
  wc.lpszClassName = L"wnd_window_class";
  RegisterClassW(&wc); // fails harmlessly on a reopen: the class survives

  // w and h are points; the window is created in pixels, and the system DPI
  // is the best guess for which monitor it will land on
  UINT dpi = GetDpiForSystem();
  RECT rect = {0, 0, (LONG)(w * (I32)dpi / 96), (LONG)(h * (I32)dpi / 96)};
  DWORD style = WS_OVERLAPPEDWINDOW;
  AdjustWindowRectExForDpi(&rect, style, 0, 0, dpi); // client size -> outer size

  HWND hwnd = 0;
  {
    ArenaTemp scratch = arena_get_scratch(0, 0);
    WCHAR* title_w = wnd__wide_from_str8(scratch.arena, title);
    hwnd = CreateWindowExW(0, wc.lpszClassName, title_w, style,
                           CW_USEDEFAULT, CW_USEDEFAULT,
                           rect.right - rect.left, rect.bottom - rect.top,
                           0, 0, instance, 0);
    arena_release_scratch(scratch);
  }
  AssertAlways(hwnd != 0); // no window: nothing sensible to do but stop

  wnd_state.hwnd = hwnd;
  wnd_state.hdc = GetDC(hwnd);
  wnd_state.is_open = 1;
  ShowWindow(hwnd, SW_SHOW);
}

internal void wnd_close(void) {
  if(wnd_state.is_open) {
    if(wnd_state.hglrc != 0) {
      wglMakeCurrent(0, 0);
      wglDeleteContext(wnd_state.hglrc);
    }
    ReleaseDC(wnd_state.hwnd, wnd_state.hdc);
    DestroyWindow(wnd_state.hwnd);
    MemoryZeroStruct(&wnd_state);
  }
}

internal F32 wnd_scale(void) {
  F32 result = 1.0f;
  if(wnd_state.is_open) {
    result = (F32)GetDpiForWindow(wnd_state.hwnd) / 96.0f;
  }
  return result;
}

internal V2 wnd_size_px(void) {
  V2 result = {0};
  if(wnd_state.is_open) {
    RECT rect = {0};
    GetClientRect(wnd_state.hwnd, &rect);
    result.x = (F32)(rect.right - rect.left);
    result.y = (F32)(rect.bottom - rect.top);
  }
  return result;
}

internal V2 wnd_size(void) {
  V2 px = wnd_size_px();
  F32 scale = wnd_scale();
  V2 result = {px.x / scale, px.y / scale};
  return result;
}

internal F32 wnd_refresh_rate(void) {
  F32 result = 0;
  if(wnd_state.is_open) {
    // called per frame, so the display-settings query hides behind a monitor
    // cache -- a drag between mixed-refresh monitors must switch the answer,
    // and the HMONITOR changing is exactly that moment
    HMONITOR monitor = MonitorFromWindow(wnd_state.hwnd, MONITOR_DEFAULTTONEAREST);
    if(monitor != wnd_state.refresh_monitor) {
      wnd_state.refresh_monitor = monitor;
      wnd_state.refresh_hz = 0;
      MONITORINFOEXW info = {0};
      info.cbSize = sizeof(info);
      if(GetMonitorInfoW(monitor, (MONITORINFO*)&info)) {
        DEVMODEW mode = {0};
        mode.dmSize = sizeof(mode);
        // dmDisplayFrequency rounds fractional rates (119.98 reads as 119 or
        // 120); the snap tolerance absorbs the difference
        if(EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) &&
           mode.dmDisplayFrequency > 1) {
          wnd_state.refresh_hz = (F32)mode.dmDisplayFrequency;
        }
      }
    }
    result = wnd_state.refresh_hz;
  }
  return result;
}

////////////////////////////////
//~ fp: OpenGL
//
// The modern-context dance: a throwaway legacy context exists only to reach
// wglGetProcAddress, which needs a current context to answer at all; then the
// real 4.1 core context replaces it. 4.1 core because that is what the
// shaders (#version 410) want and what mac maxes out at -- one GL everywhere.
// GL entry points past 1.1 load later, in render.c's r_init.

// from WGL_ARB_create_context / WGL_EXT_swap_control; spelled inline to keep
// khronos headers out of the build
#define WGL_CONTEXT_MAJOR_VERSION_ARB    0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB    0x2092
#define WGL_CONTEXT_PROFILE_MASK_ARB     0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x0001
typedef HGLRC (WINAPI* PFN_wglCreateContextAttribsARB)(HDC hdc, HGLRC share, const int* attribs);
typedef BOOL  (WINAPI* PFN_wglSwapIntervalEXT)(int interval);

internal void wnd_equip_gl(void) {
  HDC hdc = wnd_state.hdc;

  PIXELFORMATDESCRIPTOR pfd = {0};
  pfd.nSize = sizeof(pfd);
  pfd.nVersion = 1;
  pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
  pfd.iPixelType = PFD_TYPE_RGBA;
  pfd.cColorBits = 32; // no depth, no stencil: the renderer paints back-to-front
  int format = ChoosePixelFormat(hdc, &pfd);
  AssertAlways(format != 0);
  SetPixelFormat(hdc, format, &pfd);

  HGLRC legacy = wglCreateContext(hdc);
  AssertAlways(legacy != 0);
  wglMakeCurrent(hdc, legacy);

  PFN_wglCreateContextAttribsARB wgl_create_context_attribs =
    (PFN_wglCreateContextAttribsARB)(void*)wglGetProcAddress("wglCreateContextAttribsARB");
  AssertAlways(wgl_create_context_attribs != 0);

  int attribs[] = {
    WGL_CONTEXT_MAJOR_VERSION_ARB, 4,
    WGL_CONTEXT_MINOR_VERSION_ARB, 1,
    WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
    0,
  };
  HGLRC hglrc = wgl_create_context_attribs(hdc, 0, attribs);
  AssertAlways(hglrc != 0);
  wglMakeCurrent(hdc, hglrc);
  wglDeleteContext(legacy);

  // vsync, so wnd_swap paces the main loop like the other backends
  wnd_state.swap_interval_proc = (void*)wglGetProcAddress("wglSwapIntervalEXT");
  wnd_state.hglrc = hglrc;
  wnd_set_swap_interval(1);
}

internal void wnd_set_swap_interval(I32 interval) {
  PFN_wglSwapIntervalEXT wgl_swap_interval =
    (PFN_wglSwapIntervalEXT)wnd_state.swap_interval_proc;
  if(wgl_swap_interval != 0) { wgl_swap_interval(interval); }
}

internal void wnd_swap(void) {
  SwapBuffers(wnd_state.hdc);
  wnd__frame_mark();
}

#endif // OS_WINDOWS

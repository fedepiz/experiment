
#include "base/core.h"
#include "base/math.h"
#include "base/arena.h"
#include "base/tctx.h"
#include "base/strings.h"
#include "gfx/window.h"

// The OS_WINDOWS test makes this file empty for clangd, which parses it alone
// on each platform, and which has no windows.h on another platform. A real
// build includes this file on Windows only, through gfx/window.c.
#if OS_WINDOWS

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

////////////////////////////////
//~ fp: Win32 Backend
//
// The backend holds one window, and the queue of messages drives it. The
// WndProc has a place to put an event only while wnd__get_events reads that
// queue. The state therefore holds the arena and the list of that read, and
// holds them for that time alone. A message that arrives at another time,
// which occurs mostly inside CreateWindowEx, goes to DefWindowProc with no
// translation.
//
// The Win32 API uses physical pixels. The process asks for the awareness of
// the DPI of each monitor. Without that request Windows reports a wrong size,
// and stretches the image of the swapchain. Each public function converts
// through wnd_scale, which is the pixels for each point, and which is 1.5 at
// 150%. This interface therefore uses points, as the Mac backend does.

typedef struct {
  HWND hwnd;
  HDC hdc;     // CS_OWNDC makes it private, and it stays valid for the life of the GL context.
  HGLRC hglrc;
  Arena* evt_arena;        // It is not 0 while wnd__get_events reads the queue, and is 0 at each other time.
  WND_EventList* evt_list;
  HMONITOR refresh_monitor; // the key of the cache for refresh_hz. A change of monitor asks the system again.
  F32 refresh_hz;
  void* swap_interval_proc; // PFN_wglSwapIntervalEXT, from the equip. It is 0 where the driver has no such function.
  B32 is_open;
} WND_State;

global WND_State wnd_state;

// This function does the work of os_win__wide_from_str8. The two are separate,
// because the helper functions of the os backend belong to that backend, and
// the two layers stay independent.
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

      //- The modifiers. The wparam of a plain WM_KEYDOWN is already
      //  VK_SHIFT, VK_CONTROL or VK_MENU, which name no side. This backend
      //  therefore needs no step to join the left key and the right key, which
      //  the other backends have.
      case VK_SHIFT:     result = WND_Key_Shift; break;
      case VK_CONTROL:   result = WND_Key_Ctrl; break;
      case VK_MENU:      result = WND_Key_Alt; break;

      //- The punctuation. Each VK_OEM_ name is a position on a keyboard of
      //  the United States, as in the kVK table.
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
  // GetKeyState gives the state at the message that this code reads now, which
  // is the state at the time of the event. The other backends give that same
  // state.
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
      // The bit 30 of the lparam of a down message says that the key was
      // already down. Such a message is an automatic repeat, and not a change
      // of state. A KeyDown event is a change of state.
      B32 is_repeat = is_down && (lparam & (1 << 30));
      if(key != WND_Key_Nil && !is_repeat) {
        WND_Event* event = wnd__push_event(arena, list, is_down ?
                                           WND_EventType_KeyDown : WND_EventType_KeyUp);
        event->key = key;
        event->modifiers = wnd__modifiers_now();
      }
      // A system key, which is Alt with another key, and F10, goes to
      // DefWindowProc. The menu loop of that function then removes the next
      // key press after a press of Alt alone. This program has no menu, so
      // this code keeps each such key. Alt+F4 is the one exception, because
      // DefWindowProc turns it into WM_CLOSE.
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
      // Capture the mouse while a button is down, so that an up message
      // arrives after a drag that leaves the window. X11 captures without a
      // request, and the Mac backend does this work in cocoa.m.
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
      // The position of a wheel message is relative to the screen. Each other
      // mouse message gives a position relative to the client area.
      POINT p = {(I32)(I16)LOWORD(lparam), (I32)(I16)HIWORD(lparam)};
      ScreenToClient(hwnd, &p);
      // One step of the wheel is WHEEL_DELTA. A step away from the person is
      // above 0, which follows the rule for y, and a step to the right is
      // above 0 for x.
      F32 steps = (F32)GET_WHEEL_DELTA_WPARAM(wparam) / (F32)WHEEL_DELTA;
      WND_Event* event = wnd__push_event(arena, list, WND_EventType_Scroll);
      if(msg == WM_MOUSEWHEEL) { event->scroll.y = steps; }
      else                     { event->scroll.x = steps; }
      event->pos.x = (F32)p.x / scale;
      event->pos.y = (F32)p.y / scale;
      event->modifiers = wnd__modifiers_now();
    } break;

    case WM_SIZE: {
      // This message arrives at a change of size alone, because a move
      // arrives as WM_MOVE. This code therefore needs no test for a repeat. It
      // ignores the client area of 0x0 that a minimize gives, because no
      // layout can use that size.
      if(wparam != SIZE_MINIMIZED) {
        WND_Event* event = wnd__push_event(arena, list, WND_EventType_Resize);
        event->size.x = (F32)LOWORD(lparam) / scale;
        event->size.y = (F32)HIWORD(lparam) / scale;
      }
    } break;

    case WM_CLOSE: {
      // Do not call DestroyWindow here. Report the event, and let the
      // application decide. The application calls wnd_close when it stops.
      wnd__push_event(arena, list, WND_EventType_CloseRequested);
    } break;

    default: {
      result = DefWindowProcW(hwnd, msg, wparam, lparam);
    } break;
  }
  return result;
}

internal WND_EventList wnd__get_events(Arena* arena) {
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
  // Call this before a window exists, so that the first window knows the DPI.
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

  HINSTANCE instance = GetModuleHandleW(0);
  WNDCLASSW wc = {0};
  wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
  wc.lpfnWndProc = wnd__window_proc;
  wc.hInstance = instance;
  wc.hCursor = LoadCursorW(0, (LPCWSTR)IDC_ARROW);
  wc.lpszClassName = L"wnd_window_class";
  RegisterClassW(&wc); // A second open fails here and does no harm, because the class stays.

  // w and h are points. The window takes a size in pixels. The DPI of the
  // system is the best estimate, because the monitor that receives the window
  // is not known yet.
  UINT dpi = GetDpiForSystem();
  RECT rect = {0, 0, (LONG)(w * (I32)dpi / 96), (LONG)(h * (I32)dpi / 96)};
  DWORD style = WS_OVERLAPPEDWINDOW;
  AdjustWindowRectExForDpi(&rect, style, 0, 0, dpi); // from the size of the client area to the size of the whole window

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
  AssertAlways(hwnd != 0); // Without a window there is no other path, so the program stops.

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
    // A caller calls this function in each frame, so a cache holds the result
    // of the query of the display settings. A drag between two monitors of
    // different rates must change the answer, and the change of the HMONITOR
    // is that moment.
    HMONITOR monitor = MonitorFromWindow(wnd_state.hwnd, MONITOR_DEFAULTTONEAREST);
    if(monitor != wnd_state.refresh_monitor) {
      wnd_state.refresh_monitor = monitor;
      wnd_state.refresh_hz = 0;
      MONITORINFOEXW info = {0};
      info.cbSize = sizeof(info);
      if(GetMonitorInfoW(monitor, (MONITORINFO*)&info)) {
        DEVMODEW mode = {0};
        mode.dmSize = sizeof(mode);
        // dmDisplayFrequency rounds a rate that is not an integer: 119.98
        // reads as 119 or as 120. The tolerance of the step above absorbs that
        // difference.
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
// The steps that make a modern context. First the code makes an old context,
// which exists only to reach wglGetProcAddress. That function needs a current
// context to give an answer. The real context, which is 4.1 core, then takes
// the place of the old one. The version is 4.1 core because the shaders ask
// for it, with "#version 410", and because a Mac gives 4.1 at most. One
// version therefore serves each platform. Each GL entry point after 1.1 loads
// later, in r_init in render.c.

// These constants come from WGL_ARB_create_context and WGL_EXT_swap_control.
// They are in this file, so that the build needs no header of Khronos.
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
  pfd.cColorBits = 32; // There is no depth buffer and no stencil buffer, because the renderer draws from the back to the front.
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

  // Turn the vertical sync on, so that wnd_swap sets the rate of the main
  // loop, as it does with the other backends.
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

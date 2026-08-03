
#include "base/core.h"
#include "base/math.h"
#include "base/arena.h"
#include "base/tctx.h"
#include "base/strings.h"
#include "window.h"

////////////////////////////////
//~ fp: X11 Backend
//
// Xlib pollutes the TU with typedefs and macros (Window, Display, True, None,
// ...) -- in a unity build that pollution is global, so avoid those names for
// our own symbols.

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

typedef struct {
  Display* display;
  Window window;
  Atom wm_delete_window;
  V2 last_size; // last size seen, to filter move-only ConfigureNotify events
  B32 is_open;
} WND_State;

global WND_State wnd_state;

internal void wnd_open(String8 title, I32 w, I32 h) {
  Display* display = XOpenDisplay(0);
  AssertAlways(display != 0); // no X server: nothing sensible to do but stop

  int screen = DefaultScreen(display);
  Window window = XCreateSimpleWindow(display, RootWindow(display, screen),
                                      0, 0, (unsigned)w, (unsigned)h, 0,
                                      BlackPixel(display, screen),
                                      BlackPixel(display, screen));

  XSelectInput(display, window,
               KeyPressMask | KeyReleaseMask |
               ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
               StructureNotifyMask);

  // title (Xlib wants a C string -- scratch bridges as usual)
  {
    ArenaTemp scratch = arena_get_scratch(0, 0);
    String8 title_z = push_str8_copy(scratch.arena, title);
    XStoreName(display, window, (char*)title_z.str);
    arena_release_scratch(scratch);
  }

  // ask the WM to tell us about the close button instead of killing us
  Atom wm_delete_window = XInternAtom(display, "WM_DELETE_WINDOW", 0);
  XSetWMProtocols(display, window, &wm_delete_window, 1);

  XMapWindow(display, window);
  XFlush(display);

  wnd_state.display = display;
  wnd_state.window = window;
  wnd_state.wm_delete_window = wm_delete_window;
  wnd_state.last_size.x = (F32)w;
  wnd_state.last_size.y = (F32)h;
  wnd_state.is_open = 1;
}

internal void wnd_close(void) {
  if(wnd_state.is_open) {
    XDestroyWindow(wnd_state.display, wnd_state.window);
    XCloseDisplay(wnd_state.display);
    MemoryZeroStruct(&wnd_state);
  }
}

internal V2 wnd_size(void) {
  V2 result = {0};
  if(wnd_state.is_open) {
    Window root_ignored;
    int x_ignored, y_ignored;
    unsigned int w, h, border_ignored, depth_ignored;
    XGetGeometry(wnd_state.display, wnd_state.window, &root_ignored,
                 &x_ignored, &y_ignored, &w, &h, &border_ignored, &depth_ignored);
    result.x = (F32)w;
    result.y = (F32)h;
  }
  return result;
}

////////////////////////////////
//~ fp: Event Translation

internal WND_Key wnd__key_from_keysym(KeySym sym) {
  WND_Key result = WND_Key_Nil;
  if(XK_a <= sym && sym <= XK_z) { result = (WND_Key)(WND_Key_A + (sym - XK_a)); }
  else if(XK_A <= sym && sym <= XK_Z) { result = (WND_Key)(WND_Key_A + (sym - XK_A)); }
  else if(XK_0 <= sym && sym <= XK_9) { result = (WND_Key)(WND_Key_0 + (sym - XK_0)); }
  else if(XK_F1 <= sym && sym <= XK_F12) { result = (WND_Key)(WND_Key_F1 + (sym - XK_F1)); }
  else {
    switch(sym) {
      case XK_Left:      result = WND_Key_Left; break;
      case XK_Right:     result = WND_Key_Right; break;
      case XK_Up:        result = WND_Key_Up; break;
      case XK_Down:      result = WND_Key_Down; break;
      case XK_Escape:    result = WND_Key_Escape; break;
      case XK_space:     result = WND_Key_Space; break;
      case XK_Return:    result = WND_Key_Enter; break;
      case XK_Tab:       result = WND_Key_Tab; break;
      case XK_BackSpace: result = WND_Key_Backspace; break;
      case XK_Delete:    result = WND_Key_Delete; break;
      case XK_Insert:    result = WND_Key_Insert; break;
      case XK_Home:      result = WND_Key_Home; break;
      case XK_End:       result = WND_Key_End; break;
      case XK_Prior:     result = WND_Key_PageUp; break;
      case XK_Next:      result = WND_Key_PageDown; break;
      case XK_Shift_L:   case XK_Shift_R:   result = WND_Key_Shift; break;
      case XK_Control_L: case XK_Control_R: result = WND_Key_Ctrl; break;
      case XK_Alt_L:     case XK_Alt_R:     result = WND_Key_Alt; break;
      case XK_minus:     result = WND_Key_Minus; break;
      case XK_equal:     result = WND_Key_Equals; break;
      case XK_comma:     result = WND_Key_Comma; break;
      case XK_period:    result = WND_Key_Period; break;
      case XK_slash:     result = WND_Key_Slash; break;
      case XK_grave:     result = WND_Key_Backtick; break;
    }
  }
  return result;
}

internal WND_Modifiers wnd__modifiers_from_x11_state(unsigned int state) {
  WND_Modifiers result = 0;
  if(state & ShiftMask)   { result |= WND_Modifier_Shift; }
  if(state & ControlMask) { result |= WND_Modifier_Ctrl; }
  if(state & Mod1Mask)    { result |= WND_Modifier_Alt; }
  return result;
}

internal WND_Event* wnd__push_event(Arena* arena, WND_EventList* list, WND_EventType type) {
  WND_Event* event = push_array(arena, WND_Event, 1);
  event->type = type;
  SLLQueuePush(list->first, list->last, event);
  list->count += 1;
  return event;
}

internal WND_EventList wnd_get_events(Arena* arena) {
  WND_EventList list = {0};
  if(!wnd_state.is_open) { return list; }

  Display* display = wnd_state.display;
  while(XPending(display) > 0) {
    XEvent xev = {0};
    XNextEvent(display, &xev);
    switch(xev.type) {
      case KeyPress:
      case KeyRelease: {
        KeySym sym = XLookupKeysym(&xev.xkey, 0);
        WND_Key key = wnd__key_from_keysym(sym);
        if(key != WND_Key_Nil) {
          WND_EventType type = (xev.type == KeyPress) ? WND_EventType_KeyDown : WND_EventType_KeyUp;
          WND_Event* event = wnd__push_event(arena, &list, type);
          event->key = key;
          event->modifiers = wnd__modifiers_from_x11_state(xev.xkey.state);
        }
      } break;

      case ButtonPress:
      case ButtonRelease: {
        unsigned int b = xev.xbutton.button;
        WND_Modifiers modifiers = wnd__modifiers_from_x11_state(xev.xbutton.state);
        if(b == Button1 || b == Button2 || b == Button3) {
          WND_EventType type = (xev.type == ButtonPress) ? WND_EventType_MouseDown : WND_EventType_MouseUp;
          WND_Event* event = wnd__push_event(arena, &list, type);
          event->button = (b == Button1) ? WND_MouseButton_Left :
                          (b == Button2) ? WND_MouseButton_Middle :
                                           WND_MouseButton_Right;
          event->pos.x = (F32)xev.xbutton.x;
          event->pos.y = (F32)xev.xbutton.y;
          event->modifiers = modifiers;
        } else if(xev.type == ButtonPress && Button4 <= b && b <= 7) {
          // X11 encodes the wheel as buttons: 4/5 vertical, 6/7 horizontal;
          // only the press matters, the release is noise
          WND_Event* event = wnd__push_event(arena, &list, WND_EventType_Scroll);
          event->scroll.y = (b == Button4) ? 1.0f : (b == Button5) ? -1.0f : 0;
          event->scroll.x = (b == 6) ? -1.0f : (b == 7) ? 1.0f : 0;
          event->pos.x = (F32)xev.xbutton.x;
          event->pos.y = (F32)xev.xbutton.y;
          event->modifiers = modifiers;
        }
      } break;

      case MotionNotify: {
        WND_Event* event = wnd__push_event(arena, &list, WND_EventType_MouseMoved);
        event->pos.x = (F32)xev.xmotion.x;
        event->pos.y = (F32)xev.xmotion.y;
        event->modifiers = wnd__modifiers_from_x11_state(xev.xmotion.state);
      } break;

      case ConfigureNotify: {
        // fires for moves too; only report actual size changes
        V2 size = {(F32)xev.xconfigure.width, (F32)xev.xconfigure.height};
        if(size.x != wnd_state.last_size.x || size.y != wnd_state.last_size.y) {
          wnd_state.last_size = size;
          WND_Event* event = wnd__push_event(arena, &list, WND_EventType_Resize);
          event->size = size;
        }
      } break;

      case ClientMessage: {
        if((Atom)xev.xclient.data.l[0] == wnd_state.wm_delete_window) {
          wnd__push_event(arena, &list, WND_EventType_CloseRequested);
        }
      } break;
    }
  }
  return list;
}

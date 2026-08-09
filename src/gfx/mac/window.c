
#include "base/core.h"
#include "base/math.h"
#include "base/arena.h"
#include "base/tctx.h"
#include "base/strings.h"
#include "gfx/window.h"
#include "gfx/mac/cocoa.h"

////////////////////////////////
//~ fp: Cocoa Backend
//
// Cocoa needs Objective-C, and the unity translation unit stays C. The
// Objective-C code is therefore behind the layer in mac/cocoa.h and
// mac/cocoa.m, which is its own translation unit, and which x.sh links.
//
// This file is in the unity build, as each other backend is. It converts the
// values of that boundary into the style of this codebase: a raw kVK key code
// becomes a WND_Key, and an event of the boundary becomes a WND_Event on an
// arena.
//
// Each size and each position is in points, and not in pixels. On a screen of
// a high density one point is two pixels. The orientation is the orientation
// of X11, from the top left corner.
//
// The OS_MAC test makes this file empty for clangd, which parses it alone on
// each platform. A real build includes this file on a Mac only, through
// gfx/window.c.

#if OS_MAC

internal void wnd_open(String8 title, I32 w, I32 h) {
  cocoa_window_open((const char*)title.str, (int)title.size, (int)w, (int)h);
}

internal void wnd_close(void) {
  cocoa_window_close();
}

internal V2 wnd_size(void) {
  float w = 0, h = 0;
  cocoa_window_size(&w, &h);
  V2 result = {(F32)w, (F32)h};
  return result;
}

////////////////////////////////
//~ fp: OpenGL

internal void wnd_equip_gl(void) {
  cocoa_gl_equip();
}

internal void wnd_swap(void) {
  cocoa_gl_swap();
  wnd__frame_mark();
}

internal V2 wnd_size_px(void) {
  float w = 0, h = 0;
  cocoa_framebuffer_size(&w, &h);
  V2 result = {(F32)w, (F32)h};
  return result;
}

internal F32 wnd_scale(void) {
  return (F32)cocoa_backing_scale();
}

internal void wnd_set_swap_interval(I32 interval) {
  cocoa_gl_set_swap_interval((int)interval);
}

internal F32 wnd_refresh_rate(void) {
  return (F32)cocoa_refresh_rate();
}

////////////////////////////////
//~ fp: Event Translation

// The hardware key codes. Each kVK_ name is in HIToolbox/Events.h of Carbon.
// The numbers below are hexadecimal, so that Carbon stays outside this file
// and outside the compatibility layer.
internal WND_Key wnd__key_from_keycode(U16 code) {
  WND_Key result = WND_Key_Nil;
  switch(code) {
    //- the letters, which are the kVK_ANSI_ codes
    case 0x00: result = WND_Key_A; break;
    case 0x0B: result = WND_Key_B; break;
    case 0x08: result = WND_Key_C; break;
    case 0x02: result = WND_Key_D; break;
    case 0x0E: result = WND_Key_E; break;
    case 0x03: result = WND_Key_F; break;
    case 0x05: result = WND_Key_G; break;
    case 0x04: result = WND_Key_H; break;
    case 0x22: result = WND_Key_I; break;
    case 0x26: result = WND_Key_J; break;
    case 0x28: result = WND_Key_K; break;
    case 0x25: result = WND_Key_L; break;
    case 0x2E: result = WND_Key_M; break;
    case 0x2D: result = WND_Key_N; break;
    case 0x1F: result = WND_Key_O; break;
    case 0x23: result = WND_Key_P; break;
    case 0x0C: result = WND_Key_Q; break;
    case 0x0F: result = WND_Key_R; break;
    case 0x01: result = WND_Key_S; break;
    case 0x11: result = WND_Key_T; break;
    case 0x20: result = WND_Key_U; break;
    case 0x09: result = WND_Key_V; break;
    case 0x0D: result = WND_Key_W; break;
    case 0x07: result = WND_Key_X; break;
    case 0x10: result = WND_Key_Y; break;
    case 0x06: result = WND_Key_Z; break;

    //- the digits, which are the kVK_ANSI_0 to kVK_ANSI_9 codes
    case 0x1D: result = WND_Key_0; break;
    case 0x12: result = WND_Key_1; break;
    case 0x13: result = WND_Key_2; break;
    case 0x14: result = WND_Key_3; break;
    case 0x15: result = WND_Key_4; break;
    case 0x17: result = WND_Key_5; break;
    case 0x16: result = WND_Key_6; break;
    case 0x1A: result = WND_Key_7; break;
    case 0x1C: result = WND_Key_8; break;
    case 0x19: result = WND_Key_9; break;

    //- the function keys
    case 0x7A: result = WND_Key_F1; break;
    case 0x78: result = WND_Key_F2; break;
    case 0x63: result = WND_Key_F3; break;
    case 0x76: result = WND_Key_F4; break;
    case 0x60: result = WND_Key_F5; break;
    case 0x61: result = WND_Key_F6; break;
    case 0x62: result = WND_Key_F7; break;
    case 0x64: result = WND_Key_F8; break;
    case 0x65: result = WND_Key_F9; break;
    case 0x6D: result = WND_Key_F10; break;
    case 0x67: result = WND_Key_F11; break;
    case 0x6F: result = WND_Key_F12; break;

    //- the arrows
    case 0x7B: result = WND_Key_Left; break;
    case 0x7C: result = WND_Key_Right; break;
    case 0x7E: result = WND_Key_Up; break;
    case 0x7D: result = WND_Key_Down; break;

    //- the keys that edit and control
    case 0x35: result = WND_Key_Escape; break;
    case 0x31: result = WND_Key_Space; break;
    case 0x24: result = WND_Key_Enter; break;
    case 0x30: result = WND_Key_Tab; break;
    case 0x33: result = WND_Key_Backspace; break; // kVK_Delete is the key that removes the character before the cursor
    case 0x75: result = WND_Key_Delete; break;    // kVK_ForwardDelete
    case 0x72: result = WND_Key_Insert; break;    // kVK_Help is at the position of Insert on a keyboard of a PC
    case 0x73: result = WND_Key_Home; break;
    case 0x77: result = WND_Key_End; break;
    case 0x74: result = WND_Key_PageUp; break;
    case 0x79: result = WND_Key_PageDown; break;

    //- The modifiers. The left key and the right key give one value, as in
    //  the X11 backend.
    case 0x38: case 0x3C: result = WND_Key_Shift; break;
    case 0x3B: case 0x3E: result = WND_Key_Ctrl; break;
    case 0x3A: case 0x3D: result = WND_Key_Alt; break; // Option

    //- the punctuation
    case 0x1B: result = WND_Key_Minus; break;
    case 0x18: result = WND_Key_Equals; break;
    case 0x2B: result = WND_Key_Comma; break;
    case 0x2F: result = WND_Key_Period; break;
    case 0x2C: result = WND_Key_Slash; break;
    case 0x32: result = WND_Key_Backtick; break;
  }
  return result;
}

internal WND_Modifiers wnd__modifiers_from_cocoa(unsigned int mods) {
  WND_Modifiers result = 0;
  if(mods & Cocoa_Modifier_Shift) { result |= WND_Modifier_Shift; }
  if(mods & Cocoa_Modifier_Ctrl)  { result |= WND_Modifier_Ctrl; }
  if(mods & Cocoa_Modifier_Alt)   { result |= WND_Modifier_Alt; }
  return result;
}

internal WND_EventList wnd__get_events(Arena* arena) {
  WND_EventList list = {0};
  cocoa_pump_events();
  for(Cocoa_Event cev = {0}; cocoa_next_event(&cev);) {
    switch(cev.kind) {
      case Cocoa_EventKind_KeyDown:
      case Cocoa_EventKind_KeyUp: {
        WND_Key key = wnd__key_from_keycode(cev.keycode);
        if(key != WND_Key_Nil) {
          WND_EventType type = (cev.kind == Cocoa_EventKind_KeyDown) ?
                               WND_EventType_KeyDown : WND_EventType_KeyUp;
          WND_Event* event = wnd__push_event(arena, &list, type);
          event->key = key;
          event->modifiers = wnd__modifiers_from_cocoa(cev.modifiers);
        }
      } break;

      case Cocoa_EventKind_MouseDown:
      case Cocoa_EventKind_MouseUp: {
        WND_MouseButton button = (cev.button == 0) ? WND_MouseButton_Left :
                                 (cev.button == 1) ? WND_MouseButton_Right :
                                 (cev.button == 2) ? WND_MouseButton_Middle :
                                                     WND_MouseButton_Nil;
        if(button != WND_MouseButton_Nil) {
          WND_EventType type = (cev.kind == Cocoa_EventKind_MouseDown) ?
                               WND_EventType_MouseDown : WND_EventType_MouseUp;
          WND_Event* event = wnd__push_event(arena, &list, type);
          event->button = button;
          event->pos.x = (F32)cev.x;
          event->pos.y = (F32)cev.y;
          event->modifiers = wnd__modifiers_from_cocoa(cev.modifiers);
        }
      } break;

      case Cocoa_EventKind_MouseMoved: {
        WND_Event* event = wnd__push_event(arena, &list, WND_EventType_MouseMoved);
        event->pos.x = (F32)cev.x;
        event->pos.y = (F32)cev.y;
        event->modifiers = wnd__modifiers_from_cocoa(cev.modifiers);
      } break;

      case Cocoa_EventKind_Scroll: {
        WND_Event* event = wnd__push_event(arena, &list, WND_EventType_Scroll);
        event->scroll.x = (F32)cev.scroll_x;
        event->scroll.y = (F32)cev.scroll_y;
        event->pos.x = (F32)cev.x;
        event->pos.y = (F32)cev.y;
        event->modifiers = wnd__modifiers_from_cocoa(cev.modifiers);
      } break;

      case Cocoa_EventKind_Resize: {
        WND_Event* event = wnd__push_event(arena, &list, WND_EventType_Resize);
        event->size.x = (F32)cev.width;
        event->size.y = (F32)cev.height;
      } break;

      case Cocoa_EventKind_CloseRequested: {
        wnd__push_event(arena, &list, WND_EventType_CloseRequested);
      } break;
    }
  }
  return list;
}

#endif // OS_MAC

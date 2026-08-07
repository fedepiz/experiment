#pragma once
#include "base/core.h"
#include "base/math.h"
#include "base/arena.h"
#include "base/strings.h"

////////////////////////////////
//~ fp: Window
//
// One OS window: open/close, its input events, and the GL context that
// draws into it. Everything speaks client points, top-left origin, y-down.
// wnd_size_px (the framebuffer in pixels) and wnd_scale (pixels per point;
// 2 on retina) exist for the renderer's framebuffer handshake -- nothing
// else should need them.

internal void wnd_open(String8 title, I32 w, I32 h); // w, h in points
internal void wnd_close(void);
internal V2  wnd_size(void);
internal V2  wnd_size_px(void);
internal F32 wnd_scale(void);

////////////////////////////////
//~ fp: OpenGL
//
// The window owns the GL context: equip once after wnd_open, swap once per
// frame (swap blocks to the display rate -- vsync paces the main loop).

internal void wnd_equip_gl(void);
internal void wnd_swap(void);

// vblanks per swap: 1 (the equip default) presents every refresh; 2 locks to
// every second one -- the hardware-paced way to run a 120Hz display at 60,
// as opposed to a timer cap, which beats against the vblank clock and judders
internal void wnd_set_swap_interval(I32 interval);

////////////////////////////////
//~ fp: Frame Timing
//
// Delta between the last two wnd_swap calls, in seconds -- the swap is the
// frame boundary, so under vsync this reads as the display period. 0 until
// two swaps have happened. When the display rate is known, time is paid out
// in whole display periods, with a running debt banking the difference from
// the measured wall time: presentation is quantized to vblanks, so whole
// periods are the only honest currency, and the raw unblock times (which
// wobble with driver queueing and scheduling) would read as motion jitter on
// a rock-steady display. The debt is kept within about a period, so reported
// time never drifts from wall time; stalls -- debugger pauses, window drags
// -- pay out immediately as many whole periods, and callers decide what to
// do with them.

internal F32 wnd_frame_time(void);
internal F32 wnd_frame_time_raw(void); // the unsnapped measurement, for diagnostics

// refresh rate of the display the window sits on, in Hz; 0 when the backend
// has no query yet (linux), which disables the snapping above
internal F32 wnd_refresh_rate(void);

// backend contract: every wnd_swap implementation calls this after presenting
internal void wnd__frame_mark(void);

////////////////////////////////
//~ fp: Keys & Events
//
// Events are transitions: a KeyDown means the key went down (backends filter
// OS autorepeat), a MouseDown that a button was pressed. The list is rebuilt
// on the caller's arena every wnd_get_events call.

typedef U16 WND_Key;
enum {
  WND_Key_Nil = 0,

  //- letters (contiguous, so WND_Key_A + (c - 'a') works)
  WND_Key_A, WND_Key_B, WND_Key_C, WND_Key_D, WND_Key_E, WND_Key_F,
  WND_Key_G, WND_Key_H, WND_Key_I, WND_Key_J, WND_Key_K, WND_Key_L,
  WND_Key_M, WND_Key_N, WND_Key_O, WND_Key_P, WND_Key_Q, WND_Key_R,
  WND_Key_S, WND_Key_T, WND_Key_U, WND_Key_V, WND_Key_W, WND_Key_X,
  WND_Key_Y, WND_Key_Z,

  //- digits (contiguous, so WND_Key_0 + (c - '0') works)
  WND_Key_0, WND_Key_1, WND_Key_2, WND_Key_3, WND_Key_4,
  WND_Key_5, WND_Key_6, WND_Key_7, WND_Key_8, WND_Key_9,

  //- function keys (contiguous)
  WND_Key_F1, WND_Key_F2, WND_Key_F3, WND_Key_F4, WND_Key_F5, WND_Key_F6,
  WND_Key_F7, WND_Key_F8, WND_Key_F9, WND_Key_F10, WND_Key_F11, WND_Key_F12,

  //- arrows
  WND_Key_Left, WND_Key_Right, WND_Key_Up, WND_Key_Down,

  //- editing / control
  WND_Key_Escape,
  WND_Key_Space,
  WND_Key_Enter,
  WND_Key_Tab,
  WND_Key_Backspace,
  WND_Key_Delete,
  WND_Key_Insert,
  WND_Key_Home,
  WND_Key_End,
  WND_Key_PageUp,
  WND_Key_PageDown,

  //- modifiers as keys (they get their own down/up events; the held state
  //  also rides on every event via WND_Event.modifiers)
  WND_Key_Shift,
  WND_Key_Ctrl,
  WND_Key_Alt,

  //- punctuation commonly bound in games
  WND_Key_Minus,
  WND_Key_Equals,
  WND_Key_Comma,
  WND_Key_Period,
  WND_Key_Slash,
  WND_Key_Backtick,

  WND_Key_COUNT,
};

typedef U8 WND_MouseButton;
enum {
  WND_MouseButton_Nil = 0,
  WND_MouseButton_Left,
  WND_MouseButton_Right,
  WND_MouseButton_Middle,
  WND_MouseButton_COUNT,
};

// OR'd together, hence the open-enum style is mandatory here
typedef U32 WND_Modifiers;
enum {
  WND_Modifier_Shift = (1 << 0),
  WND_Modifier_Ctrl  = (1 << 1),
  WND_Modifier_Alt   = (1 << 2),
};

typedef U8 WND_EventType;
enum {
  WND_EventType_Nil,
  WND_EventType_KeyDown,
  WND_EventType_KeyUp,
  WND_EventType_MouseDown,
  WND_EventType_MouseUp,
  WND_EventType_MouseMoved,
  WND_EventType_Scroll,
  WND_EventType_Resize,
  WND_EventType_CloseRequested, // user hit the window's close button
};

// Fat struct: every event carries all fields, `type` says which mean anything.
typedef struct WND_Event WND_Event;
struct WND_Event {
  WND_EventType type;
  WND_Event* next;
  V2 pos;    // mouse position, in client points (mouse events)
  V2 scroll; // wheel delta (Scroll); +y is away from the user
  V2 size;   // new client size (Resize)
  WND_Modifiers modifiers; // held modifiers at event time (key + mouse events)
  WND_Key key;
  WND_MouseButton button;
};

typedef struct {
  U64 count;
  WND_Event* first;
  WND_Event* last;
} WND_EventList;

internal WND_EventList wnd_get_events(Arena* arena);

// shared by the backends' event pumps (implemented in gfx/window.c)
internal WND_Event* wnd__push_event(Arena* arena, WND_EventList* list, WND_EventType type);

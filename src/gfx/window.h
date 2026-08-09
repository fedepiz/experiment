#pragma once
#include "base/core.h"
#include "base/math.h"
#include "base/arena.h"
#include "base/strings.h"

////////////////////////////////
//~ fp: Window
//
// This layer holds one window of the operating system: the open and the close,
// the input events of that window, and the GL context that draws into it.
//
// Each coordinate is in the points of the client area, from the top left
// corner, with y that grows toward the bottom.
//
// wnd_size_px gives the framebuffer in pixels, and wnd_scale gives the pixels
// for each point, which is 2 on a screen of a high density. The renderer needs
// those two values to set up its framebuffer. No other code needs them.

internal void wnd_open(String8 title, I32 w, I32 h); // w and h are in points
internal void wnd_close(void);
internal V2  wnd_size(void);
internal V2  wnd_size_px(void);
internal F32 wnd_scale(void);

////////////////////////////////
//~ fp: OpenGL
//
// The window owns the GL context. Call wnd_equip_gl one time after wnd_open,
// and call wnd_swap one time in each frame. The swap waits for the display, so
// the vertical sync sets the rate of the main loop.

internal void wnd_equip_gl(void);
internal void wnd_swap(void);

// The vertical blanks for each swap. A value of 1, which the equip sets,
// presents at each refresh. A value of 2 presents at each second refresh,
// which runs a display of 120Hz at 60. The hardware makes that rate. A limit
// from a timer does not, because the timer and the vertical blank run at rates
// that are near but not equal, and the small difference between the two shows
// as an uneven motion.
internal void wnd_set_swap_interval(I32 interval);

////////////////////////////////
//~ fp: Frame Timing
//
// The seconds between the last two calls to wnd_swap. The swap is the border
// between two frames, so with a vertical sync this value is the period of the
// display. It is 0 until two swaps occur.
//
// Where the rate of the display is known, this function gives whole periods of
// the display, and holds the difference from the measured time as a debt. The
// display presents at a vertical blank, so a whole period is the only correct
// unit. A raw measurement moves with the queue of the driver and with the
// scheduler, and that movement shows as an uneven motion on a display whose
// rate does not change.
//
// The debt stays inside approximately one period, so the reported time follows
// the time of the clock. A stall, such as a pause in a debugger or a drag of
// the window, pays out at once as a number of whole periods. The caller
// chooses what to do with such a frame.

internal F32 wnd_frame_time(void);
internal F32 wnd_frame_time_raw(void); // the measurement before the step above, for a diagnostic

// The refresh rate in Hz of the display that holds the window. It is 0 where
// the backend has no such query, which is the case on Linux. A value of 0
// stops the step above.
internal F32 wnd_refresh_rate(void);

// Each backend must call this function in wnd_swap, after it presents.
internal void wnd__frame_mark(void);

////////////////////////////////
//~ fp: Keys & Events
//
// An event is a change of state. A KeyDown says that the key went down, and
// each backend removes the automatic repeat of the operating system. A
// MouseDown says that a button went down.
//
// The list joins a backend to the digest below. Each call to wnd__get_events
// builds the list again, on the arena of the caller. A caller reads the digest
// and not the list.
//
// Put here each thing that needs the order of the events, and that the digest
// cannot hold: the text that a person types, and a record of a session to
// replay.

typedef U16 WND_Key;
enum {
  WND_Key_Nil = 0,

  //- the letters. They are contiguous, so WND_Key_A + (c - 'a') gives a key.
  WND_Key_A, WND_Key_B, WND_Key_C, WND_Key_D, WND_Key_E, WND_Key_F,
  WND_Key_G, WND_Key_H, WND_Key_I, WND_Key_J, WND_Key_K, WND_Key_L,
  WND_Key_M, WND_Key_N, WND_Key_O, WND_Key_P, WND_Key_Q, WND_Key_R,
  WND_Key_S, WND_Key_T, WND_Key_U, WND_Key_V, WND_Key_W, WND_Key_X,
  WND_Key_Y, WND_Key_Z,

  //- the digits. They are contiguous, so WND_Key_0 + (c - '0') gives a key.
  WND_Key_0, WND_Key_1, WND_Key_2, WND_Key_3, WND_Key_4,
  WND_Key_5, WND_Key_6, WND_Key_7, WND_Key_8, WND_Key_9,

  //- the function keys, which are contiguous
  WND_Key_F1, WND_Key_F2, WND_Key_F3, WND_Key_F4, WND_Key_F5, WND_Key_F6,
  WND_Key_F7, WND_Key_F8, WND_Key_F9, WND_Key_F10, WND_Key_F11, WND_Key_F12,

  //- the arrows
  WND_Key_Left, WND_Key_Right, WND_Key_Up, WND_Key_Down,

  //- the keys that edit and control
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

  //- The modifiers, as keys. Each one makes its own down event and up event.
  //  Each other event also carries the modifiers that are down, in
  //  WND_Event.modifiers.
  WND_Key_Shift,
  WND_Key_Ctrl,
  WND_Key_Alt,

  //- the punctuation that a game binds most
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

// The code puts these values together with an OR, so this enum must stay open.
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
  WND_EventType_CloseRequested, // the person pressed the close button of the window
};

// Each event carries each member of this struct. `type` says which of them
// hold a value.
typedef struct WND_Event WND_Event;
struct WND_Event {
  WND_EventType type;
  WND_Event* next;
  V2 pos;    // the position of the mouse, in the points of the client area, for a mouse event
  V2 scroll; // the change of the wheel, for a Scroll. A y above 0 goes away from the person.
  V2 size;   // the new size of the client area, for a Resize
  WND_Modifiers modifiers; // the modifiers that were down at the event, for a key event and a mouse event
  WND_Key key;
  WND_MouseButton button;
};

typedef struct {
  U64 count;
  WND_Event* first;
  WND_Event* last;
} WND_EventList;

// Each backend must implement this function. It reads the queue of the
// operating system, and gives back the events that it made.
internal WND_EventList wnd__get_events(Arena* arena);

// The backends share this function. gfx/window.c implements it.
internal WND_Event* wnd__push_event(Arena* arena, WND_EventList* list, WND_EventType type);

////////////////////////////////
//~ fp: Input
//
// The digest of the stream of events, which stays equal across a frame.
// wnd_poll reads the queue one time. The state of each key and each edge then
// reads the same in each place, for the whole frame.
//
// `pressed` and `released` hold their value for the frame. A button that went
// down at any point of the frame therefore reads as pressed, and it does so
// after it comes up again inside that same frame.
//
// Each query gives the state as of the last wnd_poll. No other function reads
// the queue of the operating system.

internal void wnd_poll(void); // one time in each frame, before each query below

internal B32 wnd_close_requested(void); // the close button went down in this frame
internal B32 wnd_key_down(WND_Key key);
internal B32 wnd_key_pressed(WND_Key key);
internal B32 wnd_mouse_down(WND_MouseButton button);
internal B32 wnd_mouse_pressed(WND_MouseButton button);
internal B32 wnd_mouse_released(WND_MouseButton button);
internal V2  wnd_mouse_pos(void);
internal V2  wnd_scroll(void); // the change of the wheel across this frame. A y above 0 goes away from the person.

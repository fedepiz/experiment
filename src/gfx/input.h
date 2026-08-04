#pragma once
#include "base/core.h"
#include "base/math.h"
#include "gfx/window.h"

////////////////////////////////
//~ fp: Input
//
// Frame-coherent digest of the window's event stream: feed one frame's
// events in, then query held state and edges anywhere, all frame long.
// Pressed is a latch: a button that went down at any point in the frame
// reads as pressed even if it came back up within that same frame.

typedef struct {
  B8 key_current[WND_Key_COUNT];
  B8 key_prev[WND_Key_COUNT];
  B8 key_pressed[WND_Key_COUNT]; // latch: went down at any point this frame
  B8 mouse_current[WND_MouseButton_COUNT];
  B8 mouse_prev[WND_MouseButton_COUNT];
  B8 mouse_pressed[WND_MouseButton_COUNT]; // latch, same idea
  V2 mouse_pos;
} Input;

internal void input_process_events(Input* input, WND_EventList event_list);
internal B32  input_is_key_down(Input* input, WND_Key key);
internal B32  input_is_key_pressed(Input* input, WND_Key key);
internal B32  input_is_mouse_button_down(Input* input, WND_MouseButton mouse);
internal B32  input_is_mouse_button_pressed(Input* input, WND_MouseButton mouse);
internal V2   input_mouse_pos(Input* input);

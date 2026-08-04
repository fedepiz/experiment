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
//
// One window, one event stream, one digest -- the state is a module-internal
// global like every other gfx module, so queries need no context threaded
// through call sites. (Replay/recording, if ever wanted, belongs at the
// WND_EventList level -- events are the ground truth, this is derived.)

internal void input_process_events(WND_EventList event_list);
internal B32  input_is_key_down(WND_Key key);
internal B32  input_is_key_pressed(WND_Key key);
internal B32  input_is_mouse_button_down(WND_MouseButton mouse);
internal B32  input_is_mouse_button_pressed(WND_MouseButton mouse);
internal V2   input_mouse_pos(void);

#pragma once

#include "base/core.h"
#include "game/game.h"

typedef struct {
  B32 reload;
  B32 quit;
  V2 translation;
  F32 zooming;
  GM_MapModeFlags map_mode_toggle;
  B32 toggle_pause;
} CL_Command;


internal void cl_cmd_from_keyboard(CL_Command* cmd);


////////////////////////////////
//~ fp: FPS Counter
//
// This counter reads wnd_frame_time. Call the update one time in each frame,
// and read the value at any time. The value changes four times in each second.
// The value of 1/dt of one frame changes too fast for a person to read.

typedef struct {
  F32 accum_time;
  U32 accum_frames;
  F32 display; // the frames for each second, as of the last change of this value
} CL_FPS_Counter;

internal void hcl_fps_update(CL_FPS_Counter* counter);


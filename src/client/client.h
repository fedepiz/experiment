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
// Rides on wnd_frame_time(): update once per frame, read whenever. The
// readout refreshes four times a second -- instantaneous 1/dt flickers too
// fast to read.

typedef struct {
  F32 accum_time;
  U32 accum_frames;
  F32 display; // frames per second, as of the last refresh
} CL_FPS_Counter;

internal void hcl_fps_update(CL_FPS_Counter* counter);


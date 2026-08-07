#pragma once

#include "base/core.h"
#include "game/game.h"

typedef struct {
  B32 reload;
  B32 quit;
  V2 translation;
  F32 zooming;
  GM_MapModeFlags map_mode_toggle;
} CL_Command;



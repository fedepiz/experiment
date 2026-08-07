#pragma once
#include "base/core.h"
#include "base/math.h"
#include "base/strings.h"
#include "game/game.h"
#include "gfx/draw.h"
#include "tabula.h"
#include "ui.h"
#include "client/client.h"

////////////////////////////////
//~ fp: HUD
//
// The seam between the pure UI core and gfx: text callbacks answered by
// draw, input gathered into a UI_Input, the frame's draw list replayed
// through d_* calls. Fonts cross the boundary as D_Font.u64. hud_build:
// the selection panel, a placeholder action bar, the FPS readout.

//- fp: the ui_init callbacks and per-frame crossings
internal V2 hud_measure_text(void* user, U64 font, F32 size, String8 text);
internal UI_FontMetrics hud_font_metrics(void* user, U64 font, F32 size);
internal UI_Input hud_gather_input(void);
internal void hud_replay_draw_list(UI_DrawList list);

internal void hud_build(D_Font font, TB_Value* info, CL_Command* command);

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
// This module joins the UI core, which uses the base layer alone, to the gfx
// layer. The draw layer answers the two functions that measure a text. This
// module reads the input into a UI_Input, and draws the list of commands of
// the frame with the d_ functions. A font crosses that boundary as the u64 of
// a D_Font.
//
// hud_build makes the panel of the selection, a bar of actions that stands in
// place of the true one, and the counter of the frames.

//- fp: the two functions for ui_init, and the calls of each frame
internal V2 hud_measure_text(void* user, U64 font, F32 size, String8 text);
internal UI_FontMetrics hud_font_metrics(void* user, U64 font, F32 size);
internal UI_Input hud_gather_input(void);
internal void hud_replay_draw_list(UI_DrawList list);

internal void hud_build(D_Font font, TB_Value* info, CL_Command* command);

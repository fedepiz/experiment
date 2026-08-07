#include "base/core.h"
#include "base/math.h"
#include "base/arena.h"
#include "base/strings.h"
#include "base/tctx.h"
#include "tabula.h"
#include "game/game.h"
#include "gfx/window.h"
#include "gfx/input.h"
#include "gfx/draw.h"
#include "ui.h"
#include "client/client.h"
#include "client/hud.h"

////////////////////////////////
//~ fp: FPS Counter

internal void hud_fps_update(FPS_Counter* counter) {
  counter->accum_time += wnd_frame_time();
  counter->accum_frames += 1;
  if(counter->accum_time >= 0.25f) {
    counter->display = (F32)counter->accum_frames / counter->accum_time;
    counter->accum_time = 0;
    counter->accum_frames = 0;
  }
}

////////////////////////////////

internal V2 hud_measure_text(void* user, U64 font, F32 size, String8 text) {
  Unused(user);
  return d_text_dim((D_Font){font}, size, text);
}

internal UI_FontMetrics hud_font_metrics(void* user, U64 font, F32 size) {
  Unused(user);
  D_FontMetrics m = d_font_metrics((D_Font){font}, size);
  return (UI_FontMetrics){m.ascent, m.descent, m.line_advance};
}

// the mouse arrives in client points, the UI's native unit
internal UI_Input hud_gather_input(void) {
  UI_Input in = {0};
  in.mouse = input_mouse_pos();
  WND_MouseButton buttons[UI_MouseButton_COUNT] = {
      WND_MouseButton_Left, WND_MouseButton_Right, WND_MouseButton_Middle};
  for(U32 i = 0; i < UI_MouseButton_COUNT; i += 1) {
    in.down[i] = (B8)input_is_mouse_button_down(buttons[i]);
    in.pressed[i] = (B8)input_is_mouse_button_pressed(buttons[i]);
    in.released[i] = (B8)input_is_mouse_button_released(buttons[i]);
  }
  in.scroll = input_scroll();
  in.dt = wnd_frame_time();
  in.window = wnd_size();
  return in;
}

internal void hud_replay_draw_list(UI_DrawList list) {
  for(U64 i = 0; i < list.count; i += 1) {
    UI_DrawCommand* cmd = &list.commands[i];
    switch(cmd->kind) {
      case UI_DrawCommandKind_Rect: {
        D_RectParams params = {0};
        params.rect = cmd->rect;
        for(Corner c = 0; c < Corner_COUNT; c += 1) {
          params.colors[c] = cmd->colors[c];
          params.corner_radii[c] = cmd->corner_radii[c];
        }
        params.border_thickness = cmd->border_thickness;
        params.edge_softness = cmd->edge_softness;
        d_rect_ex(&params);
      } break;
      case UI_DrawCommandKind_Text: {
        d_text((D_Font){cmd->font}, cmd->font_size, cmd->pos, cmd->color, cmd->text);
      } break;
      case UI_DrawCommandKind_ClipPush: {
        d_push_clip(cmd->rect);
      } break;
      case UI_DrawCommandKind_ClipPop: {
        d_pop_clip();
      } break;
      default: break;
    }
  }
}

// one label/value line: label in ink at the left, value at the right
internal void hud__stat_row(String8 label, String8 value, V4 value_color) {
  UI_Box* row = ui_row_begin(label);
  row->pref_size[UI_Axis_X] = ui_size_pct(1, 0);
  ui_label(label);
  ui_spacer(ui_size_grow(1));
  ui_push_text_color(value_color);
  ui_label(value);
  ui_pop_text_color();
  ui_row_end();
}

// the generic tier of the selection panel: any member of the fact tree
// renders without hud changes. Scalars become stat rows (numbers in accent),
// lists join into one value, nested objects become muted section headers
// followed by their own rows. `omit` skips one top-level key (the curated
// title). Strings live on the caller's arena for the duration of the build.
internal void hud__fact_rows(TB_Value* object, String8 omit) {
  ArenaTemp scratch = arena_get_scratch(0, 0);
  for(TB_Node* node = object->first_member; node != 0; node = node->next) {
    if(omit.size > 0 && str8_match(node->key, omit, 0)) { continue; }
    TB_Value* value = &node->value;
    switch(value->kind) {
      case TB_ValueKind_Identifier:
      case TB_ValueKind_String: {
        hud__stat_row(node->key, value->text, ui_color_from_name(str8_lit("text")));
      } break;
      case TB_ValueKind_Number: {
        hud__stat_row(node->key, value->text, ui_color_from_name(str8_lit("accent")));
      } break;
      case TB_ValueKind_List: {
        String8List parts = {0};
        for(TB_Value* el = value->first; el != 0; el = el->next) {
          str8_list_push(scratch.arena, &parts, el->text);
        }
        StringJoin join = {str8_lit(""), str8_lit(", "), str8_lit("")};
        String8 value = str8_list_join(scratch.arena, parts, &join);
        hud__stat_row(node->key, value, ui_color_from_name(str8_lit("text")));
      } break;
      case TB_ValueKind_Object: {
        ui_spacer(ui_size_points(2, 1));
        ui_push_text_color(ui_color_from_name(str8_lit("muted")));
        ui_label(node->key);
        ui_pop_text_color();
        hud__fact_rows(value, str8_lit(""));
      } break;
      default: break;
    }
  }
  arena_release_scratch(scratch);
}

// top-left panel over the current selection: curated title (thing name, or
// the tile's terrain), then the generic fact rows
internal void hud_selection_panel(TB_Value* info) {
  if(!tb_value_is_nil(info)) {
    ui_push_padding((V2){12, 10});
    UI_Box* panel = ui_panel_begin(str8_lit("selection_panel"));
    ui_pop_padding();
    panel->flags |= UI_BoxFlag_Floating;
    panel->floating_anchor = (V2){0, 0};
    panel->floating_pos = (V2){12, 12};
    panel->pref_size[UI_Axis_X] = ui_size_points(240, 1);
    {
      String8 title = tb_get_str8(info, str8_lit("name"), str8_lit(""));
      if(title.size == 0) {
        title = tb_get_str8(tb_get(info, str8_lit("tile")), str8_lit("terrain"), str8_lit("tile"));
      }
      ui_push_font_size(19);
      ui_label(title);
      ui_pop_font_size();
      hud__fact_rows(info, str8_lit("name"));
    }
    ui_panel_end();
  }
}

// full-width bar along the bottom edge; the buttons are placeholders. The
// bar is a strip, not a card: fixed height, horizontal inset only
internal void hud_action_bar(CL_Command* cmd) {
  ui_push_padding((V2){10, 0});
  UI_Box* bar = ui_panel_begin(str8_lit("action_bar"));
  ui_pop_padding();
  bar->flags |= UI_BoxFlag_Floating;
  bar->floating_anchor = (V2){0, 1};
  bar->pref_size[UI_Axis_X] = ui_size_pct(1, 1);
  bar->pref_size[UI_Axis_Y] = ui_size_points(40, 1);
  ui_push_pref_height(ui_size_grow(1));
  ui_push_child_align(UI_Align_Center);
  UI_Row(str8_lit("actions")) {
    if(ui_button(str8_lit("Toggle Ifluence")).clicked) {
      cmd->map_mode_toggle |= GM_MapModeFlag_Influence;
    }
  }
  ui_pop_child_align();
  ui_pop_pref_height();
  ui_panel_end();
}

// top-right corner: the readout fps_update refreshes
internal void hud_fps_panel(FPS_Counter* fps) {
  ui_push_padding((V2){8, 4});
  UI_Box* panel = ui_panel_begin(str8_lit("fps_panel"));
  ui_pop_padding();
  panel->flags |= UI_BoxFlag_Floating;
  panel->floating_anchor = (V2){1, 0};
  panel->floating_pos = (V2){-12, 12};
  ui_labelf("FPS %.1f", fps->display);
  ui_panel_end();
}

internal void hud_build(D_Font font, FPS_Counter* fps, TB_Value* info, CL_Command* cmd) {
  ui_push_font(font.u64);
  ui_push_font_size(15);
  ui_push_child_gap(8);

  hud_selection_panel(info);
  hud_action_bar(cmd);
  hud_fps_panel(fps);

  ui_pop_child_gap();
  ui_pop_font_size();
  ui_pop_font();
}

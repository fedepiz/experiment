
#include "client/map.c"
#include "client/hud.c"
#include "client/report.c"
#include "gfx/window.h"

internal void cl_cmd_from_keyboard(CL_Command* cmd) {
  struct {
    WND_Key key;
    F32 x;
    F32 y;
    F32 z;
  } INTERACTIONS[] = {
      {WND_Key_A, -1, 0, 0},
      {WND_Key_D, 1, 0, 0},
      {WND_Key_W, 0, -1, 0},
      {WND_Key_S, 0, 1, 0},
      {WND_Key_Q, 0, 0, 1},
      {WND_Key_E, 0, 0, -1}};

  V2 d_trans = {0};
  F32 d_zoom = 0;

  for(U32 i = 0; i < ArrayCount(INTERACTIONS); ++i) {
    if(wnd_key_down((INTERACTIONS[i].key))) {
      d_trans.x += INTERACTIONS[i].x;
      d_trans.y += INTERACTIONS[i].y;
      d_zoom += INTERACTIONS[i].z;
    }
  }

  cmd->translation = v2_add(cmd->translation, d_trans);
  cmd->zooming += d_zoom;
  cmd->reload = wnd_key_pressed(WND_Key_R);
  cmd->quit = wnd_key_pressed(WND_Key_Escape);

  cmd->toggle_pause |= wnd_key_pressed(WND_Key_Space);
  if(wnd_key_pressed(WND_Key_1)) {
    cmd->map_mode_toggle |= GM_MapModeFlag_Influence;
  }
}

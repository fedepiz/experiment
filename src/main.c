////////////////////////////////
//~ fp: Layer Includes
//
// The only file the compiler is pointed at. All headers first, then all
// implementations -- that ordering is why the .h/.c split still earns its keep
// in a unity build.

//- fp: [h]
#include "base/arena.h"
#include "base/base.h"
#include "base/core.h"
#include "base/math.h"
#include "base/print.h"
#include "base/strings.h"
#include "base/tctx.h"
#include "game/board.h"
#include "game/thing_db.h"
#include "game/tiling.h"
#include "tabula.h"
#include "game/game.h"
#include "gfx/color.h"
#include "gfx/window.h"
#include "gfx/input.h"
#include "gfx/draw.h"
#include "ui.h"
#include "client/client.h"
#include "client/map.h"
#include "client/hud.h"
#include "client/report.h"

////////////////////////////////
//~ fp: temp: Test Render
//
// The draw-layer demo scene, parked: call it from inside the frame in place
// of (or after) map_render whenever the draw layer needs a re-check. Owns its
// assets and loads them on first call.

internal void test_render(D_Camera camera) {
  local_persist B32 initialized = 0;
  local_persist D_Font font;
  local_persist D_Sprite checker;
  local_persist D_Sprite stripes;
  local_persist F32 stripes_angle = 0;
  if(!initialized) {
    initialized = 1;
    font = d_font_open(str8_lit("assets/fonts/Arial.ttf"));
    ArenaTemp scratch = arena_get_scratch(0, 0);
    enum { SPRITE_DIM = 128 };
    D_Image checker_img = d_image_create(scratch.arena, SPRITE_DIM, SPRITE_DIM, (V4){0});
    D_Image stripes_img = d_image_create(scratch.arena, SPRITE_DIM, SPRITE_DIM, (V4){0});
    for(I32 y = 0; y < SPRITE_DIM; y += 1) {
      for(I32 x = 0; x < SPRITE_DIM; x += 1) {
        B32 c_on = (((x >> 4) + (y >> 4)) & 1);
        d_image_set_px(checker_img, x, y, c_on ? (V4){0.90f, 0.35f, 0.78f, 1} : (V4){0.16f, 0.16f, 0.19f, 1});
        B32 s_on = (((x + y) >> 4) & 1);
        d_image_set_px(stripes_img, x, y, s_on ? (V4){0.24f, 0.82f, 0.75f, 1} : (V4){0.08f, 0.24f, 0.24f, 1});
      }
    }
    d_spritesheet_begin(512, 256, D_Sampling_Smooth);
    checker = d_spritesheet_push(checker_img);
    stripes = d_spritesheet_push(stripes_img);
    d_spritesheet_end();
    arena_release_scratch(scratch); // pixels are on the GPU now
  }

  V2 vp = wnd_size();
  d_rect_gradient_v((Rect){{0, 0}, {vp.x, vp.y}},
                    (V4){0.09f, 0.09f, 0.13f, 1}, (V4){0.03f, 0.03f, 0.05f, 1});

  d_rect_rounded((Rect){{100, 100}, {500, 300}}, (V4){0.2f, 0.4f, 0.9f, 1}, 24);
  d_rect_outline((Rect){{100, 350}, {500, 550}}, (V4){1, 1, 1, 1}, 4);

  d_sprite(checker, (Rect){{550, 100}, {806, 356}}, (V4){0}); // zero tint = as-is
  stripes_angle += 0.5f * wnd_frame_time();                   // half a radian per second
  D_SpriteParams sp = {0};
  sp.sprite = stripes;
  sp.dst = (Rect){{880, 120}, {1080, 320}};
  sp.rotation = stripes_angle;
  d_sprite_ex(&sp);

  d_push_clip((Rect){{1150, 120}, {1350, 320}});
  d_rect((Rect){{1100, 100}, {1500, 340}}, (V4){0.9f, 0.4f, 0.8f, 1});
  d_pop_clip();

  d_line((V2){550, 420}, (V2){1050, 480}, 3, (V4){1.0f, 0.8f, 0.2f, 1});

  d_camera_begin(camera);
  d_rect_rounded((Rect){{-60, 120}, {60, 200}}, (V4){0.2f, 0.8f, 0.4f, 1}, 10);
  d_camera_end();

  d_text(font, 40, (V2){100, 600}, Col_White,
         str8_lit("Imperium â€” draw layer online"));
  d_text(font, 18, (V2){100, 660}, col_rgb(0.7f, 0.7f, 0.75f),
         str8_lit("rects, sprites, sheets, clip, camera, text"));

  // hue ramp via col_hsva, fading out via col_with_alpha
  for(I32 i = 0; i < 24; i += 1) {
    F32 t = (F32)i / 24.0f;
    d_rect((Rect){{100 + (F32)i * 30, 720}, {128 + (F32)i * 30, 760}},
           col_with_alpha(col_hsva(t, 0.85f, 1.0f, 1.0f), 1.0f - t * 0.7f));
  }
}

////////////////////////////////
//~ fp: 60fps Present Pacing
//
// Hardware-paces presentation to 60fps: when the monitor runs at a whole
// multiple of 60 (60Hz, 120Hz, ...), present every Nth vblank -- the display
// clock then enforces an exact 60 on every screen, and a drag between
// mixed-rate monitors re-resolves through the cached wnd_refresh_rate.
// Non-multiples (144Hz, 90Hz) fall back to every vblank; the +-2Hz tolerance
// absorbs fractional rates (119.98 reads as 120). Call once per frame.

internal void pace_60fps_update(void) {
  local_persist I32 current_interval = 0;
  F32 hz = wnd_refresh_rate();
  I32 multiple = (I32)(hz / 60.0f + 0.5f);
  I32 interval = 1;
  if(multiple >= 1 &&
     60.0f * (F32)multiple - 2.0f < hz && hz < 60.0f * (F32)multiple + 2.0f) {
    interval = multiple;
  }
  if(interval != current_interval) {
    current_interval = interval;
    wnd_set_swap_interval(interval);
  }
}

////////////////////////////////
//~ fp: Camera
//
// Steering lives here, not in client/map: the map is pure presentation, and
// what keys mean is the shell's decision. The camera itself is a D_Camera
// value the map only ever receives.

internal void camera_update(D_Camera* camera, V2 translation, F32 zooming) {
  F32 dt = wnd_frame_time();
  F32 zoom = (camera->zoom == 0) ? 1.0f : camera->zoom;

  //- inertial movement: the keys steer a target velocity, and the camera's
  //  velocity exponentially chases it -- the same curve accelerates on press
  //  and glides to a stop on release. `blend` is the fraction of the
  //  remaining gap closed this frame; putting dt in the exponent makes two
  //  half-frames compose to exactly one whole one, so the feel survives any
  //  framerate. Bigger rate = snappier (velocity half-life = 1/rate seconds).
  F32 blend = 1.0f - f32_exp2(-8.0f * dt);

  // pan target is world-units-per-second scaled by 1/zoom, so panning covers
  // a constant fraction of the screen at any zoom level; the zoom target is
  // in doublings per second -- exponential, so it reads as the same speed at
  // every level (the 1/zoom factor belongs to panning alone)
  V2 pan_target = v2_scale(translation, 400.0f / zoom);
  F32 zoom_target = 3.0f * zooming;
  camera->pan_vel = v2_add(camera->pan_vel, v2_scale(v2_sub(pan_target, camera->pan_vel), blend));
  camera->zoom_vel += (zoom_target - camera->zoom_vel) * blend;

  camera->center = v2_scaled_add(camera->center, camera->pan_vel, dt);
  zoom *= f32_exp2(camera->zoom_vel * dt);
  camera->zoom = Clamp(0.25f, zoom, 8.0f);
}

internal void cmd_from_keyboard(CL_Command* cmd) {
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
    if(input_is_key_down((INTERACTIONS[i].key))) {
      d_trans.x += INTERACTIONS[i].x;
      d_trans.y += INTERACTIONS[i].y;
      d_zoom += INTERACTIONS[i].z;
    }
  }

  cmd->translation = v2_add(cmd->translation, d_trans);
  cmd->zooming += d_zoom;
  cmd->reload = input_is_key_pressed(WND_Key_R);
  cmd->quit = input_is_key_pressed(WND_Key_Escape);

  if(input_is_key_pressed(WND_Key_1)) {
    cmd->map_mode_toggle |= GM_MapModeFlag_Influence;
  }
}

////////////////////////////////
//~ fp: Entry Point

// decimal digits only -- anything else is a usage error, not a number
internal B32 main__parse_u64(String8 s, U64* out) {
  if(s.size == 0) { return 0; }
  U64 value = 0;
  for(U64 i = 0; i < s.size; i += 1) {
    U8 c = s.str[i];
    if(c < '0' || c > '9') { return 0; }
    value = value * 10 + (c - '0');
  }
  *out = value;
  return 1;
}

int main(int argc, char** argv) {
  TCTX tctx;
  tctx_init_and_equip(&tctx);

  //- fp: temp: headless report mode exits before any window exists
  if(argc >= 3 && str8_match(str8_cstring(argv[1]), str8_lit("worlds"), 0)) {
    U64 world_count = 0;
    U64 first_seed = 1;
    B32 ok = main__parse_u64(str8_cstring(argv[2]), &world_count);
    if(argc >= 4) { ok = ok && main__parse_u64(str8_cstring(argv[3]), &first_seed); }
    if(!ok) {
      eprintf_str8("usage: %s worlds N [first_seed]\n", argv[0]);
      tctx_release();
      return 1;
    }
    report_worldgen((I32)world_count, first_seed);
    tctx_release();
    return 0;
  }

  Arena* frame_arena = arena_alloc();
  wnd_open(str8_lit("Imperium"), 1600, 900);
  wnd_equip_gl();
  d_init();
  ui_init(hud_measure_text, hud_font_metrics, 0);
  ui_set_theme(ui_theme_load(arena_alloc(), str8_lit("data/ui.tabula")));

  //- fp: the long-lived game state; the map view shadows it (its cell cache
  //  is rebuilt with it on every reseed)
  Arena* game_arena = arena_alloc();
  GM_Game game = {0};
  U64 game_next_seed = 2704;

  // the terrain table drives asset naming and tiling registration
  wg_terrain_table_load(str8_lit("data/world.tabula"));
  Map_View* map = map_init(arena_alloc()); // outlives every reseed

  D_Font hud_font = d_font_open(str8_lit("assets/fonts/Arial.ttf"));
  FPS_Counter fps_counter = {0};

  D_Camera camera = {0};
  B32 hud_mouse_over = 0; // as of the last built frame

  GM_MapModeFlags map_mode_flags = GM_MapModeFlag_Pawns;

  // the frame, in phases: pump events -> input digest -> key/click handling
  // -> pacing -> lazy (re)init -> sim -> draw (map, then HUD) -> camera ->
  // swap. camera_update runs AFTER drawing on purpose: a frame draws with
  // the same camera value the click handling above saw, one frame stale --
  // the hud_mouse_over convention.
  for(B32 keep_going = true; keep_going;) {
    WND_EventList evts = wnd_get_events(frame_arena);
    for(WND_Event* evt = evts.first; evt; evt = evt->next) {
      if(evt->type == WND_EventType_CloseRequested) {
        keep_going = false;
      }
    }

    input_process_events(evts);

    CL_Command cmd = {0};
    cmd_from_keyboard(&cmd);


    UI_DrawList hud_list = {0};
    {
      TB_Value* info = gm_selection_info(frame_arena, &game);
      hud_fps_update(&fps_counter);
      ui_frame_begin(frame_arena, hud_gather_input());
      hud_build(hud_font, &fps_counter, info, &cmd);
      hud_list = ui_frame_end();
    }

    if(cmd.quit) { keep_going = false; }

    if(cmd.reload) { game.initialised = false; }

    if(game.initialised && !hud_mouse_over) {
      if(input_is_mouse_button_pressed(WND_MouseButton_Left)) {
        gm_select(&game, map_tile_from_screen(camera, input_mouse_pos()));
      }
      if(input_is_mouse_button_pressed(WND_MouseButton_Right)) {
        gm_deselect(&game);
      }
    }

    map_mode_flags ^= cmd.map_mode_toggle;

    pace_60fps_update();

    if(!game.initialised) {
      arena_clear(game_arena);
      gm_init(game_arena, &game, game_next_seed++);
      map_world_changed(map, game_arena, game.board->width, game.board->height);
      // Reset the camera
      camera.center = (V2){game.board->width * MAP_TILE / 2, game.board->height * MAP_TILE / 2};
      camera.zoom = 1.0f; // whole map in view; scroll to zoom in
    }

    gm_update(&game, wnd_frame_time());

    d_frame_begin(frame_arena, wnd_size_px(), wnd_scale());
    {
      GM_MapMode mode = {0};
      // visible tile window: the cells under the screen corners, padded one
      // ring out. The far edge takes one MORE ring: each cell owns the dual
      // boundary cell at its NW corner (tiling.h), so the duals past the
      // last board column/row need an owner -- the trailing +1 lands on the
      // ring cells gm_map_items emits for exactly that.
      I32 board_w = game.board->width;
      I32 board_h = game.board->height;
      V2I tile_min = map_tile_from_screen(camera, (V2){0, 0});
      V2I tile_max = map_tile_from_screen(camera, wnd_size());
      mode.min = (V2){ClampBot(tile_min.x - 1, 0), ClampBot(tile_min.y - 1, 0)};
      mode.max = (V2){ClampTop(tile_max.x + 1, board_w - 1) + 1,
                      ClampTop(tile_max.y + 1, board_h - 1) + 1};

      mode.flags = map_mode_flags;
      GM_MapItems map_items = gm_map_items(frame_arena, &game, mode);
      map_draw(map, map_items, camera);
      // test_render(camera); // fp: parked draw-layer demo scene

      hud_replay_draw_list(hud_list);
      hud_mouse_over = hud_list.mouse_over_ui;
    }
    d_frame_end();

    camera_update(&camera, cmd.translation, cmd.zooming); // after drawing: see the phase comment above the loop

    wnd_swap(); // vsync: paces the loop to the display rate
    arena_clear(frame_arena);
  }
  wnd_close();

  tctx_release();
  return 0;
}

//- fp: [c]
#include "base/base.c"
#include "tabula.c"
#include "ui.c"
#include "game/tiling.c"
#include "game/game.c"
#include "gfx/gfx.c"
#include "client/client.c"

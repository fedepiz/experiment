////////////////////////////////
//~ fp: Layer Includes
//
// This is the one file that the compiler reads. It includes each header first,
// and then each implementation. That order is the reason why the split of a
// module into a .h file and a .c file is useful in a unity build.

//- fp: [h]
#include "base/arena.h"
#include "base/base.h"
#include "base/core.h"
#include "base/math.h"
#include "base/print.h"
#include "base/strings.h"
#include "base/tctx.h"
#include "gfx/color.h"
#include "gfx/window.h"
#include "gfx/draw.h"
#include "game/board.h"
#include "game/thing_db.h"
#include "game/tiling.h"
#include "game/game.h"
#include "ui.h"
#include "client/client.h"
#include "client/map.h"
#include "client/hud.h"
#include "client/report.h"

////////////////////////////////
//~ fp: temp: Test Render
//
// A scene that shows what the draw layer does. No code calls it now. Call it
// inside the frame, in place of map_render or after it, to test the draw layer
// again. It owns its assets, and it loads them at its first call.

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
    arena_release_scratch(scratch); // the pixels are on the GPU now
  }

  V2 vp = wnd_size();
  d_rect_gradient_v((Rect){{0, 0}, {vp.x, vp.y}},
                    (V4){0.09f, 0.09f, 0.13f, 1}, (V4){0.03f, 0.03f, 0.05f, 1});

  d_rect_rounded((Rect){{100, 100}, {500, 300}}, (V4){0.2f, 0.4f, 0.9f, 1}, 24);
  d_rect_outline((Rect){{100, 350}, {500, 550}}, (V4){1, 1, 1, 1}, 4);

  d_sprite(checker, (Rect){{550, 100}, {806, 356}}, (V4){0}); // a tint of 0 keeps the colors of the sprite
  stripes_angle += 0.5f * wnd_frame_time();                   // half a radian for each second
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

  // col_hsva moves the hue, and col_with_alpha lowers the alpha.
  for(I32 i = 0; i < 24; i += 1) {
    F32 t = (F32)i / 24.0f;
    d_rect((Rect){{100 + (F32)i * 30, 720}, {128 + (F32)i * 30, 760}},
           col_with_alpha(col_hsva(t, 0.85f, 1.0f, 1.0f), 1.0f - t * 0.7f));
  }
}

////////////////////////////////
//~ fp: 60fps Present Pacing
//
// The hardware presents the frames at 60 for each second. Where the rate of
// the monitor is a whole multiple of 60, such as 60Hz or 120Hz, the window
// presents at each Nth vertical blank. The clock of the display then gives
// exactly 60 on each screen, and a drag between two monitors of different rates
// reads wnd_refresh_rate again.
//
// Where the rate is not such a multiple, such as 144Hz or 90Hz, the window
// presents at each vertical blank. The tolerance of 2Hz absorbs a rate that is
// not an integer, so 119.98 reads as 120.
//
// Call this function one time in each frame.

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
// The code that steers the camera is here, and not in client/map, because the
// map draws and does nothing more, and because the meaning of a key is a
// decision of this file. The camera itself is a D_Camera value, which the map
// receives and does not change.

internal void camera_inertial_move(D_Camera* camera, V2 translation, F32 zooming) {
  F32 dt = wnd_frame_time();
  F32 zoom = (camera->zoom == 0) ? 1.0f : camera->zoom;

  //- The movement with inertia. The keys set a target velocity, and the
  //  velocity of the camera moves toward that target along an exponential
  //  curve. That one curve gives the increase of speed at a press and the
  //  decrease to a stop at a release.
  //
  //  `blend` is the part of the difference that this frame removes. dt is in
  //  the exponent, so two frames of a half length give the same result as one
  //  frame of a full length, and the movement therefore feels the same at each
  //  frame rate. A larger `rate` makes the camera answer faster: the velocity
  //  falls to one half in 1/rate seconds.
  F32 blend = 1.0f - f32_exp2(-8.0f * dt);

  // The target of the pan is in units of the world for each second, multiplied
  // by 1/zoom. A pan therefore covers the same part of the screen at each
  // zoom.
  //
  // The target of the zoom is in doublings for each second. That measure is
  // exponential, so a zoom feels equally fast at each level. The factor of
  // 1/zoom belongs to the pan alone.
  V2 pan_target = v2_scale(translation, 400.0f / zoom);
  F32 zoom_target = 3.0f * zooming;
  camera->pan_vel = v2_add(camera->pan_vel, v2_scale(v2_sub(pan_target, camera->pan_vel), blend));
  camera->zoom_vel += (zoom_target - camera->zoom_vel) * blend;

  camera->center = v2_scaled_add(camera->center, camera->pan_vel, dt);
  zoom *= f32_exp2(camera->zoom_vel * dt);
  camera->zoom = Clamp(0.25f, zoom, 8.0f);
}

////////////////////////////////
//~ fp: Entry Point

// The text must hold decimal digits alone. Each other text is a mistake of the
// caller, and not a number.
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

  //- fp: temp: The mode that writes a report stops the program before a window
  //  exists.
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

  //- fp: The state of the game, which exists for the run of the program and
  //  owns its arena. The view of the map follows it: a new seed builds the
  //  cache of the cells of that view again.
  GM_Game game = {0};
  U64 game_next_seed = 2704;

  // The terrain table gives the name of each asset and the registration in the
  // tiler.
  wg_terrain_table_load(str8_lit("data/terrain_types.tabula"));
  Map_View* map = map_init(arena_alloc()); // it stays across each new seed

  D_Font hud_font = d_font_open(str8_lit("assets/fonts/Arial.ttf"));
  CL_FPS_Counter fps_counter = {0};

  D_Camera camera = {0};
  B32 hud_mouse_over = 0; // as of the last frame that the code built

  GM_MapModeFlags map_mode_flags = GM_MapModeFlag_Pawns;

  // The frame, in phases: read the input, then handle the keys and the clicks,
  // then set the rate, then initialize the world where that is necessary, then
  // run the simulation, then draw the map and the HUD, then move the camera,
  // then swap.
  //
  // camera_update runs AFTER the draw, and it does so deliberately. The frame
  // therefore draws with the camera value that the code for the clicks read,
  // which is one frame old. hud_mouse_over follows that same rule.
  //
  // A request to close finishes its frame. The test of the loop reads that
  // request at the start of the next frame.
  for(B32 keep_going = true; keep_going && !wnd_close_requested();) {
    wnd_poll();

    CL_Command cmd = {0};
    cl_cmd_from_keyboard(&cmd);


    UI_DrawList hud_list = {0};
    {
      TB_Value* info = gm_info(frame_arena, &game);

      {
        // add the frames for each second
        String8 text = push_str8f(frame_arena, "%.1f", fps_counter.display);
        tb_add_str8(frame_arena, info, str8_lit("fps"), text);
      }

      hcl_fps_update(&fps_counter);
      ui_frame_begin(frame_arena, hud_gather_input());
      hud_build(hud_font, info, &cmd);
      hud_list = ui_frame_end();
    }

    if(cmd.quit) { keep_going = false; }

    if(cmd.reload) { game.initialised = false; }

    if(cmd.toggle_pause) { game.paused ^= 1; }

    if(game.initialised && !hud_mouse_over) {
      if(wnd_mouse_pressed(WND_MouseButton_Left)) {
        gm_select(&game, map_tile_from_screen(camera, wnd_mouse_pos()));
      }
      if(wnd_mouse_pressed(WND_MouseButton_Right)) {
        gm_deselect(&game);
      }
    }

    map_mode_flags ^= cmd.map_mode_toggle;

    pace_60fps_update();

    if(!game.initialised) {
      // The terrain table reloads with the world, so a hot edit of the file
      // shows on the next reload. The assets of the map keep the rows of the
      // read at map_init.
      wg_terrain_table_load(str8_lit("data/terrain_types.tabula"));
      gm_init(&game, game_next_seed++);
      map_world_changed(map, game.board->width, game.board->height);
      // set the camera to its first state
      camera.center = (V2){game.board->width * MAP_TILE / 2, game.board->height * MAP_TILE / 2};
      camera.zoom = 1.0f; // the whole map is in view. The wheel makes the view smaller.
    }

    gm_update(&game, wnd_frame_time());

    d_frame_begin(frame_arena, wnd_size_px(), wnd_scale());
    {
      GM_MapMode mode = {0};
      // The window of the tiles that a person sees. It holds the cells below
      // the corners of the screen, and one ring of cells around them.
      //
      // The far edge takes one MORE ring. Each cell owns the dual cell of the
      // boundary at its corner to the north west. See tiling.h. The dual cells
      // past the last column and the last row of the board therefore need an
      // owner, and the +1 at the end reaches the ring cells that gm_map_items
      // makes for that purpose.
      I32 board_w = game.board->width;
      I32 board_h = game.board->height;
      V2I tile_min = map_tile_from_screen(camera, (V2){0, 0});
      V2I tile_max = map_tile_from_screen(camera, wnd_size());
      mode.window = rng2i32(
          (V2I){ClampBot(tile_min.x - 1, 0), ClampBot(tile_min.y - 1, 0)},
          (V2I){ClampTop(tile_max.x + 1, board_w - 1) + 1,
                ClampTop(tile_max.y + 1, board_h - 1) + 1});

      mode.flags = map_mode_flags;
      GM_MapItems map_items = gm_map_items(frame_arena, &game, mode);
      map_draw(map, map_items, camera);
      // test_render(camera); // fp: the scene that shows what the draw layer does

      hud_replay_draw_list(hud_list);
      hud_mouse_over = hud_list.mouse_over_ui;
    }
    d_frame_end();

    camera_inertial_move(&camera, cmd.translation, cmd.zooming); // It runs after the draw. See the comment on the phases above the loop.

    wnd_swap(); // The vertical sync makes the loop run at the rate of the display.
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

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
#include "base/print.h"
#include "base/strings.h"
#include "tabula.h"
#include "game/game.h"
#include "gfx/color.h"
#include "gfx/window.h"
#include "gfx/input.h"
#include "gfx/draw.h"

////////////////////////////////
//~ fp: temp: Test Map
//
// A 256x256 world to exercise board + draw together: terrain painted
// procedurally (deterministic integer-hash noise, no rng state), one road
// laid along the terrain's own best path, and a debug rendering -- colored
// tiles, a dot where a road runs, a rounded rect per pawn.

enum {
  Terrain_Plains,
  Terrain_Forest,
  Terrain_Mountain,
  Terrain_Water,
  Terrain_COUNT,
};

#define MAP_DIM  256
#define MAP_TILE 8.0f // world units per tile

global F32 map_terrain_cost[Terrain_COUNT] = {1.0f, 2.0f, 0, 0}; // mountain/water impassable
global V4 map_terrain_colors[Terrain_COUNT] = {
    {0.47f, 0.62f, 0.33f, 1}, // plains
    {0.23f, 0.42f, 0.24f, 1}, // forest
    {0.54f, 0.52f, 0.50f, 1}, // mountain
    {0.18f, 0.32f, 0.55f, 1}, // water
};
global V4 map_pawn_colors[] = {
    {0.95f, 0.80f, 0.25f, 1},
    {0.88f, 0.32f, 0.26f, 1},
    {0.93f, 0.93f, 0.96f, 1},
};

internal U32 map__noise(I32 x, I32 y) {
  U32 h = (U32)x * 374761393u + (U32)y * 668265263u;
  h ^= h >> 16;
  h *= 0x7feb352du;
  h ^= h >> 15;
  h *= 0x846ca68bu;
  h ^= h >> 16;
  return h;
}

internal BD_Board* map_create(Arena* arena) {
  BD_Board* board = bd_board_alloc(arena, MAP_DIM, MAP_DIM, 1024);
  board->rules.terrain_cost = map_terrain_cost;
  board->rules.terrain_cost_count = Terrain_COUNT;
  board->rules.road_cost = 0.5f;

  for(I32 y = 0; y < MAP_DIM; y += 1) {
    for(I32 x = 0; x < MAP_DIM; x += 1) {
      BD_Terrain terrain = Terrain_Plains;
      if(map__noise(x >> 3, y >> 3) % 100 < 35) { terrain = Terrain_Forest; }

      // north-south mountain ridge wiggling as a triangle wave, with one pass
      I32 tri = (y % 64) < 32 ? (y % 64) : 64 - (y % 64);
      I32 ridge = 128 + tri - 16;
      B32 in_pass = 120 <= y && y < 136;
      if(!in_pass && ridge - 5 <= x && x <= ridge + 5) { terrain = Terrain_Mountain; }

      // two lakes
      I32 dx0 = x - 70, dy0 = y - 170;
      I32 dx1 = x - 190, dy1 = y - 60;
      if(dx0 * dx0 + dy0 * dy0 < 25 * 25 || dx1 * dx1 + dy1 * dy1 < 18 * 18) {
        terrain = Terrain_Water;
      }

      bd_tile_at(board, (V2I){x, y})->terrain = terrain;
    }
  }

  // a road between two would-be settlements, following the terrain's own
  // best path (through the mountain pass)
  {
    ArenaTemp scratch = arena_get_scratch(0, 0);
    BD_Path path = bd_path_find(scratch.arena, board, (V2I){40, 128}, (V2I){216, 128});
    for(U64 idx = 0; idx + 1 < path.count; idx += 1) {
      V2I delta = v2i_sub(path.points[idx + 1], path.points[idx]);
      for(BD_Dir dir = 0; dir < BD_Dir_COUNT; dir += 1) {
        if(v2i_eq(bd_dir_delta(dir), delta)) {
          bd_feature_connect(board, path.points[idx], dir, BD_Feature_Road);
          break;
        }
      }
    }
    arena_release_scratch(scratch);
  }
  return board;
}

internal void map_render(BD_Board* board, D_Camera camera) {
  V2 vp = wnd_size();
  d_rect((Rect){{0, 0}, {vp.x, vp.y}}, (V4){0.06f, 0.06f, 0.08f, 1});

  d_camera_begin(camera);
  {
    // only the tiles the viewport can see
    V2 world_min = d_camera_from_screen(camera, (V2){0, 0});
    V2 world_max = d_camera_from_screen(camera, vp);
    I32 x0 = ClampBot((I32)(world_min.x / MAP_TILE) - 1, 0);
    I32 y0 = ClampBot((I32)(world_min.y / MAP_TILE) - 1, 0);
    I32 x1 = ClampTop((I32)(world_max.x / MAP_TILE) + 1, board->width - 1);
    I32 y1 = ClampTop((I32)(world_max.y / MAP_TILE) + 1, board->height - 1);

    F32 dot = MAP_TILE * 0.15f;
    for(I32 y = y0; y <= y1; y += 1) {
      for(I32 x = x0; x <= x1; x += 1) {
        BD_Tile* tile = bd_tile_at(board, (V2I){x, y});
        Rect r = {{x * MAP_TILE, y * MAP_TILE}, {(x + 1) * MAP_TILE, (y + 1) * MAP_TILE}};
        V4 color = tile->terrain < Terrain_COUNT ? map_terrain_colors[tile->terrain] : (V4){1, 0, 1, 1}; // magenta = bad id
        d_rect(r, color);
        if(tile->features[BD_Feature_Road] != 0) {
          V2 c = {(x + 0.5f) * MAP_TILE, (y + 0.5f) * MAP_TILE};
          d_rect_rounded((Rect){{c.x - dot, c.y - dot}, {c.x + dot, c.y + dot}},
                         (V4){0.42f, 0.31f, 0.20f, 1}, dot);
        }
      }
    }

    // pawns on top; their rects stay inside their tile, second pass keeps
    // the layering obvious
    F32 inset = MAP_TILE * 0.2f;
    for(I32 y = y0; y <= y1; y += 1) {
      for(I32 x = x0; x <= x1; x += 1) {
        for(BD_Pawn* pawn = bd_tile_at(board, (V2I){x, y})->first_pawn;
            pawn != 0; pawn = pawn->next) {
          Rect r = {{x * MAP_TILE + inset, y * MAP_TILE + inset},
                    {(x + 1) * MAP_TILE - inset, (y + 1) * MAP_TILE - inset}};
          d_rect_rounded(r, map_pawn_colors[pawn->kind % ArrayCount(map_pawn_colors)],
                         (MAP_TILE - 2 * inset) * 0.35f);
        }
      }
    }
  }
  d_camera_end();
}

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
    d_spritesheet_begin(512, 256);
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
         str8_lit("Imperium — draw layer online"));
  d_text(font, 18, (V2){100, 660}, col_rgb(0.7f, 0.7f, 0.75f),
         str8_lit("rects, sprites, sheets, clip, camera, text"));

  // hue ramp via col_hsva, fading out via col_with_alpha
  for(I32 i = 0; i < 24; i += 1) {
    F32 t = (F32)i / 24.0f;
    d_rect((Rect){{100 + (F32)i * 30, 720}, {128 + (F32)i * 30, 760}},
           col_with_alpha(col_hsva(t, 0.85f, 1.0f, 1.0f), 1.0f - t * 0.7f));
  }
}

internal void camera_update(D_Camera* camera) {
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
  F32 zoom = (camera->zoom == 0) ? 1.0f : camera->zoom;
  // world-units-per-second scaled by 1/zoom, so panning covers a
  // constant fraction of the screen at any zoom level
  F32 delta = wnd_frame_time() * 400.0f / zoom;

  camera->center = v2_scaled_add(camera->center, d_trans, delta);

  // scroll zooms, exponentially so steps feel even at any level
  zoom *= 1.0f + 0.1f * d_zoom;
  camera->zoom = Clamp(0.25f, zoom, 8.0f);
}

////////////////////////////////
//~ fp: temp: FPS Counter
//
// Rides on wnd_frame_time(): update once per frame, draw whenever. The
// readout refreshes four times a second -- instantaneous 1/dt flickers too
// fast to read. ZII, except that a font must be assigned for draw to show
// anything.

typedef struct {
  D_Font font;
  F32 accum_time;
  U32 accum_frames;
  F32 display; // frames per second, as of the last refresh
} FPS_Counter;

internal void fps_update(FPS_Counter* counter) {
  counter->accum_time += wnd_frame_time();
  counter->accum_frames += 1;
  if(counter->accum_time >= 0.25f) {
    counter->display = (F32)counter->accum_frames / counter->accum_time;
    counter->accum_time = 0;
    counter->accum_frames = 0;
  }
}

// black box, white text, top-center of the window
internal void fps_draw(FPS_Counter* counter) {
  ArenaTemp scratch = arena_get_scratch(0, 0);
  String8 text = push_str8f(scratch.arena, "FPS %.1f", counter->display);
  F32 text_size = 16;
  V2 pad = {8, 4};
  V2 dim = d_text_dim(counter->font, text_size, text);
  V2 vp = wnd_size();
  Rect box = {{(vp.x - dim.x) / 2 - pad.x, 8},
              {(vp.x + dim.x) / 2 + pad.x, 8 + dim.y + 2 * pad.y}};
  d_rect_rounded(box, (V4){0, 0, 0, 1}, 6);
  d_text(counter->font, text_size, (V2){box.min.x + pad.x, box.min.y + pad.y},
         Col_White, text);
  arena_release_scratch(scratch);
}

////////////////////////////////
//~ fp: Entry Point

int main(void) {
  TCTX tctx;
  tctx_init_and_equip(&tctx);

  Arena* frame_arena = arena_alloc();
  wnd_open(str8_lit("Imperium"), 1600, 900);
  wnd_equip_gl();
  d_init();

  //- fp: temp: the long-lived map, plus a few pawns wandering waypoints
  Arena* map_arena = arena_alloc();
  BD_Board* board = map_create(map_arena);

  V2I waypoints[] = {{30, 30}, {220, 40}, {210, 210}, {40, 220}};
  struct {
    BD_PawnID id;
    U32 goal;   // index into waypoints
    F32 points; // banked movement points; steps are paid at bd_step_cost
  } entities[3];
  for(U32 i = 0; i < ArrayCount(entities); i += 1) {
    entities[i].id = bd_pawn_create(board, waypoints[i], i, 0);
    entities[i].goal = (i + 1) % ArrayCount(waypoints);
    entities[i].points = 0;
  }
  F32 move_timer = 0;

  //- fp: temp: fps overlay
  FPS_Counter fps_counter = {0};
  fps_counter.font = d_font_open(str8_lit("assets/fonts/Arial.ttf"));

  D_Camera camera = {0};
  camera.center = (V2){MAP_DIM * MAP_TILE / 2, MAP_DIM * MAP_TILE / 2};
  camera.zoom = 0.5f; // whole map in view; scroll to zoom in

  for(B32 keep_going = true; keep_going;) {
    WND_EventList evts = wnd_get_events(frame_arena);
    for(WND_Event* evt = evts.first; evt; evt = evt->next) {
      if(evt->type == WND_EventType_CloseRequested) {
        keep_going = false;
      }
    }

    input_process_events(evts);

    if(input_is_key_down(WND_Key_Escape)) {
      keep_going = false;
    }

    if(input_is_mouse_button_pressed(WND_MouseButton_Left)) {
      V2 pos = input_mouse_pos();
      printf_str8("(%f, %f)\n", pos.x, pos.y);
    }

    //- fp: temp: entities bank movement points each tick and pay the
    //  board's step cost to walk, so terrain speed is felt, not just routed
    //  around: forest crossings crawl, road hops fly
    move_timer = ClampTop(move_timer + wnd_frame_time(), 0.5f);
    while(move_timer > 0.1f) {
      move_timer -= 0.1f;
      for(U32 i = 0; i < ArrayCount(entities); i += 1) {
        // 1 point per tick: a plains step; banking is capped so waiting at a
        // cheap stretch cannot buy a later teleport across an expensive one
        entities[i].points = ClampTop(entities[i].points + 1.0f, 4.0f);
        for(;;) {
          BD_Pawn* pawn = bd_pawn_from_id(entities[i].id);
          V2I goal = waypoints[entities[i].goal];
          if(v2i_eq(pawn->pos, goal)) {
            entities[i].goal = (entities[i].goal + 1) % ArrayCount(waypoints);
            goal = waypoints[entities[i].goal];
          }
          V2I next = bd_path_next_towards(board, pawn->pos, goal);
          if(v2i_eq(next, pawn->pos)) {
            // unreachable from here: skip that waypoint
            entities[i].goal = (entities[i].goal + 1) % ArrayCount(waypoints);
            break;
          }
          F32 cost = bd_step_cost(board, pawn->pos, next);
          if(cost <= 0 || entities[i].points < cost) { break; } // not affordable yet
          entities[i].points -= cost;
          bd_pawn_move(board, entities[i].id, next);
        }
      }
    }

    d_frame_begin(frame_arena, wnd_size_px(), wnd_scale());
    {
      map_render(board, camera);
      // test_render(camera); // fp: parked draw-layer demo scene

      fps_update(&fps_counter);
      fps_draw(&fps_counter);

      // Camera movmements
      camera_update(&camera);
    }
    d_frame_end();

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
#include "game/game.c"
#include "gfx/gfx.c"

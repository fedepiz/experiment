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
#include "game/board.h"
#include "tabula.h"
#include "game/game.h"
#include "gfx/color.h"
#include "gfx/window.h"
#include "gfx/input.h"
#include "gfx/draw.h"

////////////////////////////////
//~ fp: temp: Test Map
//
// The world itself comes from worldgen (knobs in data/world.tabula); this
// section is the demo glue around it: snapping demo points onto passable
// land, one road laid along the terrain's own best path, and a debug
// rendering -- colored tiles, line segments for rivers and roads, a rounded
// rect per pawn.

#define MAP_TILE 8.0f // world units per tile

global V4 MAP_PAWN_COLORS[] = {
    {0.95f, 0.80f, 0.25f, 1},
    {0.88f, 0.32f, 0.26f, 1},
    {0.93f, 0.93f, 0.96f, 1},
};

////////////////////////////////
//~ fp: temp: Map Assets
//
// Everything map_render draws from, loaded once at startup and packed into
// one runtime spritesheet. Today that is terrain tiles (tools/gen_tiles.py ->
// assets/tiles/<terrain>_<n>.png); entity sprites, feature art, and the rest
// join this struct as they get art. Variants for a terrain load until the
// first missing file; a terrain with no art at all keeps count 0 and
// map_render falls back to its flat WG_TERRAIN_DATA color, so art can arrive
// terrain by terrain.

#define MAP_TILE_VARIANTS 8

// ground tiles are world-space slices: a 4x4 seamless torus per terrain,
// indexed by map position, so neighboring tiles continue one texture
#define MAP_GROUND_GRID 4
#define MAP_GROUND_VARIANTS (MAP_GROUND_GRID * MAP_GROUND_GRID)

typedef struct {
  D_Sprite terrain_sprites[WG_TerrainType_COUNT][MAP_GROUND_VARIANTS];
  U32 terrain_variant_counts[WG_TerrainType_COUNT];

  // pictographic sprites (tree clusters, rock masses) drawn over the ground
  // tile; terrains without overlay art keep count 0 and draw ground only
  D_Sprite overlay_sprites[WG_TerrainType_COUNT][MAP_TILE_VARIANTS];
  U32 overlay_variant_counts[WG_TerrainType_COUNT];

  // sparser art for tiles on a region's border (lone trees, small rocks):
  // forests thin out and massifs crumble instead of ending in a wall
  D_Sprite edge_overlay_sprites[WG_TerrainType_COUNT][MAP_TILE_VARIANTS];
  U32 edge_overlay_variant_counts[WG_TerrainType_COUNT];

  // ground fringes (DF edge-shapes style): a terrain's ground cut by a
  // ragged mask, drawn spilling over the boundary onto the neighbor tile;
  // indexed by the side of the tile the owning neighbor lies on
  D_Sprite fringe_sprites[WG_TerrainType_COUNT][BD_Dir_COUNT][MAP_TILE_VARIANTS];
  U32 fringe_variant_counts[WG_TerrainType_COUNT][BD_Dir_COUNT];
} MapAssets;

// who spills over whom where grounds meet: higher rank fringes onto lower,
// so every boundary blends exactly once, in one direction. Zero never spills
// onto anyone; water only receives, which is exactly what makes coastlines
// read as ragged land-over-water.
global U8 MAP_FRINGE_RANK[WG_TerrainType_COUNT] = {
    0, // nil
    0, // water
    5, // plains
    4, // forest
    1, // mountain
    2, // desert
    3, // swamp
};

global char* MAP_FRINGE_DIR_NAMES[BD_Dir_COUNT] = {"n", "e", "s", "w"};

internal MapAssets map_assets_load(void) {
  MapAssets assets = {0};
  ArenaTemp scratch = arena_get_scratch(0, 0);
  d_spritesheet_begin(512, 512);
  for(WG_TerrainType type = 0; type < WG_TerrainType_COUNT; type += 1) {
    for(U32 variant = 0; variant < MAP_GROUND_VARIANTS; variant += 1) {
      String8 path = push_str8f(scratch.arena, "assets/tiles/%S_%u.png",
                                WG_TERRAIN_DATA[type].name, variant);
      D_Image image = d_image_load(scratch.arena, path);
      if(image.w == 0) { break; } // ZII: missing/broken file is the zero image
      assets.terrain_sprites[type][variant] = d_spritesheet_push(image);
      assets.terrain_variant_counts[type] += 1;
    }
    for(U32 variant = 0; variant < MAP_TILE_VARIANTS; variant += 1) {
      String8 path = push_str8f(scratch.arena, "assets/tiles/%S_overlay_%u.png",
                                WG_TERRAIN_DATA[type].name, variant);
      D_Image image = d_image_load(scratch.arena, path);
      if(image.w == 0) { break; }
      assets.overlay_sprites[type][variant] = d_spritesheet_push(image);
      assets.overlay_variant_counts[type] += 1;
    }
    for(U32 variant = 0; variant < MAP_TILE_VARIANTS; variant += 1) {
      String8 path = push_str8f(scratch.arena, "assets/tiles/%S_edge_%u.png",
                                WG_TERRAIN_DATA[type].name, variant);
      D_Image image = d_image_load(scratch.arena, path);
      if(image.w == 0) { break; }
      assets.edge_overlay_sprites[type][variant] = d_spritesheet_push(image);
      assets.edge_overlay_variant_counts[type] += 1;
    }
    for(BD_Dir dir = 0; dir < BD_Dir_COUNT; dir += 1) {
      for(U32 variant = 0; variant < MAP_TILE_VARIANTS; variant += 1) {
        String8 path = push_str8f(scratch.arena, "assets/tiles/%S_fringe_%s_%u.png",
                                  WG_TERRAIN_DATA[type].name, MAP_FRINGE_DIR_NAMES[dir], variant);
        D_Image image = d_image_load(scratch.arena, path);
        if(image.w == 0) { break; }
        assets.fringe_sprites[type][dir][variant] = d_spritesheet_push(image);
        assets.fringe_variant_counts[type][dir] += 1;
      }
    }
  }
  d_spritesheet_end();
  arena_release_scratch(scratch);
  return assets;
}

// which variant a tile shows -- pure position hash, so it never flickers
internal U32 map_variant_noise(I32 x, I32 y) {
  U32 h = (U32)x * 374761393u + (U32)y * 668265263u;
  h ^= h >> 16;
  h *= 0x7feb352du;
  h ^= h >> 15;
  h *= 0x846ca68bu;
  h ^= h >> 16;
  return h;
}

internal B32 map_tile_passable(BD_Board* board, V2I p) {
  BD_Terrain terrain = bd_tile_at(board, p)->terrain;
  return terrain < board->rules.terrain_cost_count &&
         board->rules.terrain_cost[terrain] > 0;
}

// nearest passable tile to `want`, searching outward ring by ring; `want`
// itself when the whole board is impassable (callers just get a dead pawn)
internal V2I map_snap_passable(BD_Board* board, V2I want) {
  I32 max_radius = Max(board->width, board->height);
  for(I32 radius = 0; radius < max_radius; radius += 1) {
    for(I32 dy = -radius; dy <= radius; dy += 1) {
      for(I32 dx = -radius; dx <= radius; dx += 1) {
        if(Max(dx, -dx) != radius && Max(dy, -dy) != radius) { continue; } // ring, not disc
        V2I p = {want.x + dx, want.y + dy};
        if(bd_in_bounds(board, p) && map_tile_passable(board, p)) { return p; }
      }
    }
  }
  return want;
}

internal BD_Board* map_create(Arena* arena, U64 seed) {
  WG_Params params = wg_params_load(str8_lit("data/world.tabula"));
  BD_Board* board = wg_generate(arena, &params, seed);

  // a road between two would-be settlements on opposite sides of the
  // continent, following the terrain's own best path
  {
    V2I west = map_snap_passable(board, (V2I){board->width / 6, board->height / 2});
    V2I east = map_snap_passable(board, (V2I){board->width * 5 / 6, board->height / 2});
    ArenaTemp scratch = arena_get_scratch(0, 0);
    BD_Path path = bd_path_find(scratch.arena, board, west, east);
    for(U64 idx = 0; idx + 1 < path.count; idx += 1) {
      BD_Dir dir = bd_dir_from_delta(v2i_sub(path.points[idx + 1], path.points[idx]));
      bd_feature_connect(board, path.points[idx], dir, BD_Feature_Road);
    }
    arena_release_scratch(scratch);
  }
  return board;
}

internal void map_render(BD_Board* board, MapAssets* assets, D_Camera camera) {
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

    for(I32 y = y0; y <= y1; y += 1) {
      for(I32 x = x0; x <= x1; x += 1) {
        BD_Tile* tile = bd_tile_at(board, (V2I){x, y});
        Rect r = {{x * MAP_TILE, y * MAP_TILE}, {(x + 1) * MAP_TILE, (y + 1) * MAP_TILE}};
        BD_Terrain terrain = tile->terrain < WG_TerrainType_COUNT ? tile->terrain : WG_TerrainType_Nil;
        U32 h = map_variant_noise(x, y); // ground and overlay pick from different bits
        U32 count = assets->terrain_variant_counts[terrain];
        if(count == MAP_GROUND_VARIANTS) {
          // a full set is a world-space sliced torus: index by position and
          // neighboring tiles continue one seamless texture
          U32 variant = (U32)((y & (MAP_GROUND_GRID - 1)) * MAP_GROUND_GRID +
                              (x & (MAP_GROUND_GRID - 1)));
          d_sprite(assets->terrain_sprites[terrain][variant], r, (V4){0}); // zero tint = as-is
        } else if(count > 0) {
          d_sprite(assets->terrain_sprites[terrain][h % count], r, (V4){0});
        } else {
          d_rect(r, WG_TERRAIN_DATA[terrain].color); // nil's table color is loud magenta
        }

        BD_Terrain neighbors[BD_Dir_COUNT];
        B32 on_region_border = 0;
        for(BD_Dir dir = 0; dir < BD_Dir_COUNT; dir += 1) {
          neighbors[dir] = bd_tile_at(board, v2i_add((V2I){x, y}, bd_dir_delta(dir)))->terrain;
          on_region_border |= neighbors[dir] != terrain;
        }

        // ground fringes: the dominant neighbor's ground spills a ragged
        // tongue across the boundary, so tones wander over the tile grid.
        // Water ranks 0, so shores are land fringing over water -- ragged
        // coastlines fall out of the same rule
        for(BD_Dir dir = 0; dir < BD_Dir_COUNT; dir += 1) {
          BD_Terrain neighbor = neighbors[dir];
          if(neighbor >= WG_TerrainType_COUNT || neighbor == terrain) { continue; }
          if(MAP_FRINGE_RANK[neighbor] <= MAP_FRINGE_RANK[terrain]) { continue; }
          U32 fringe_count = assets->fringe_variant_counts[neighbor][dir];
          if(fringe_count == 0) { continue; }
          U32 fh = map_variant_noise(x + (I32)dir * 131, y);
          d_sprite(assets->fringe_sprites[neighbor][dir][fh % fringe_count], r, (V4){0});
        }

        // overlays taper at the region border: sparse edge art there, full
        // art inside -- with occasional bare gaps so a massif or forest
        // reads as scattered shapes, not a carpet of one tile art per cell
        U32 overlay_count = assets->overlay_variant_counts[terrain];
        U32 edge_count = assets->edge_overlay_variant_counts[terrain];
        if(on_region_border && edge_count > 0) {
          d_sprite(assets->edge_overlay_sprites[terrain][(h >> 8) % edge_count], r, (V4){0});
        } else if(overlay_count > 0 && (h >> 16) % 100 < 80) {
          d_sprite(assets->overlay_sprites[terrain][(h >> 8) % overlay_count], r, (V4){0});
        }
      }
    }

    // features as half-segments, tile center toward each connected neighbor;
    // the neighbor draws the matching half, so connections read as one line
    // (an edge tile's off-map half sends rivers visibly off the world).
    // Rivers under roads: where both run, the road is the bridge.
    struct {
      BD_Feature feature;
      F32 thickness;
      V4 color;
    } FEATURE_STYLES[] = {
        {BD_Feature_River, MAP_TILE * 0.25f, {0.25f, 0.45f, 0.75f, 1}},
        {BD_Feature_Road, MAP_TILE * 0.18f, {0.42f, 0.31f, 0.20f, 1}},
    };
    for(U32 style_idx = 0; style_idx < ArrayCount(FEATURE_STYLES); style_idx += 1) {
      for(I32 y = y0; y <= y1; y += 1) {
        for(I32 x = x0; x <= x1; x += 1) {
          U8 mask = bd_tile_at(board, (V2I){x, y})->features[FEATURE_STYLES[style_idx].feature];
          if(mask == 0) { continue; }
          V2 center = {(x + 0.5f) * MAP_TILE, (y + 0.5f) * MAP_TILE};
          for(BD_Dir dir = 0; dir < BD_Dir_COUNT; dir += 1) {
            if(((mask >> dir) & 1) == 0) { continue; }
            V2I delta = bd_dir_delta(dir);
            V2 edge = {center.x + delta.x * MAP_TILE * 0.5f,
                       center.y + delta.y * MAP_TILE * 0.5f};
            d_line(center, edge, FEATURE_STYLES[style_idx].thickness,
                   FEATURE_STYLES[style_idx].color);
          }
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
          d_rect_rounded(r, MAP_PAWN_COLORS[pawn->kind % ArrayCount(MAP_PAWN_COLORS)],
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
  V2 pan_target = v2_scale(d_trans, 400.0f / zoom);
  F32 zoom_target = 3.0f * d_zoom;
  camera->pan_vel = v2_add(camera->pan_vel, v2_scale(v2_sub(pan_target, camera->pan_vel), blend));
  camera->zoom_vel += (zoom_target - camera->zoom_vel) * blend;

  camera->center = v2_scaled_add(camera->center, camera->pan_vel, dt);
  zoom *= f32_exp2(camera->zoom_vel * dt);
  camera->zoom = Clamp(0.25f, zoom, 8.0f);
}

////////////////////////////////
//~ fp: temp: 60fps Present Pacing
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
//~ fp: temp: Game State
//
// Everything the demo simulates, gathered on one struct: the board plus a few
// entities wandering a waypoint loop. Entities bank movement points each tick
// and pay the board's step cost to walk, so terrain speed is felt, not just
// routed around: forest crossings crawl, road hops fly.

typedef struct {
  BD_PawnID id;
  U32 goal;   // index into game->waypoints
  F32 points; // banked movement points; steps are paid at bd_step_cost
} Entity;

typedef struct {
  B32 initialised;
  BD_Board* board;
  V2I waypoints[4];
  Entity entities[3];
  F32 move_timer;
} Game;

internal void game_init(Arena* arena, Game* game, U64 seed) {
  MemoryZeroStruct(game);
  game->board = map_create(arena, seed);

  // corner-ish waypoints, snapped onto whatever land this world grew there
  V2I corners[] = {{30, 30}, {220, 40}, {210, 210}, {40, 220}};
  StaticAssert(ArrayCount(corners) == ArrayCount(game->waypoints), waypoint_count);
  for(U32 i = 0; i < ArrayCount(game->waypoints); i += 1) {
    game->waypoints[i] = map_snap_passable(game->board, corners[i]);
  }

  for(U32 i = 0; i < ArrayCount(game->entities); i += 1) {
    game->entities[i].id = bd_pawn_create(game->board, game->waypoints[i], i, 0);
    game->entities[i].goal = (i + 1) % ArrayCount(game->waypoints);
    game->entities[i].points = 0;
  }

  game->initialised = true;
}

internal void game_update(Game* game) {
  game->move_timer = ClampTop(game->move_timer + wnd_frame_time(), 0.5f);
  while(game->move_timer > 0.1f) {
    game->move_timer -= 0.1f;
    for(U32 i = 0; i < ArrayCount(game->entities); i += 1) {
      Entity* entity = &game->entities[i];
      // 1 point per tick: a plains step; banking is capped so waiting at a
      // cheap stretch cannot buy a later teleport across an expensive one
      entity->points = ClampTop(entity->points + 1.0f, 4.0f);
      for(;;) {
        BD_Pawn* pawn = bd_pawn_from_id(entity->id);
        V2I goal = game->waypoints[entity->goal];
        if(v2i_eq(pawn->pos, goal)) {
          entity->goal = (entity->goal + 1) % ArrayCount(game->waypoints);
          goal = game->waypoints[entity->goal];
        }
        V2I next = bd_path_next_towards(game->board, pawn->pos, goal);
        if(v2i_eq(next, pawn->pos)) {
          // unreachable from here: skip that waypoint
          entity->goal = (entity->goal + 1) % ArrayCount(game->waypoints);
          break;
        }
        F32 cost = bd_step_cost(game->board, pawn->pos, next);
        if(cost <= 0 || entity->points < cost) { break; } // not affordable yet
        entity->points -= cost;
        bd_pawn_move(game->board, entity->id, next);
      }
    }
  }
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

  //- fp: temp: the long-lived game state
  Arena* game_arena = arena_alloc();
  Game game = {0};
  U64 game_next_seed = 2704;

  MapAssets map_assets = map_assets_load();

  //- fp: temp: fps overlay
  FPS_Counter fps_counter = {0};
  fps_counter.font = d_font_open(str8_lit("assets/fonts/Arial.ttf"));

  D_Camera camera = {0};

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

    pace_60fps_update();

    if(!game.initialised) {
      arena_clear(game_arena);
      game_init(game_arena, &game, game_next_seed++);
      // Reset the camera
      camera.center = (V2){game.board->width * MAP_TILE / 2, game.board->height * MAP_TILE / 2};
      camera.zoom = 0.5f; // whole map in view; scroll to zoom in
    }

    game_update(&game);

    d_frame_begin(frame_arena, wnd_size_px(), wnd_scale());
    {
      map_render(game.board, &map_assets, camera); // fp: parked for the pacing experiment below
      // test_render(camera); // fp: parked draw-layer demo scene

      fps_update(&fps_counter);
      fps_draw(&fps_counter);
    }
    d_frame_end();

    // Camera movmements
    camera_update(&camera);

    if(input_is_key_pressed(WND_Key_R)) {
      game.initialised = false;
    }

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

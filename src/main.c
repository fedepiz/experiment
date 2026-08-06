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

////////////////////////////////
//~ fp: temp: Test Map
//
// The world itself is game/'s (worldgen knobs in data/world.tabula); this
// section is the presentation glue around it: sprite assets, the tiler
// cache, and a debug rendering -- tiles, line segments for rivers and
// roads, a rounded rect per pawn.

#define MAP_TILE 8.0f // world units per tile

////////////////////////////////
//~ fp: temp: Map Assets
//
// One flat sprite table plus the tiling registration built while filling it.
// The loader narrates what exists to the tiler (tl_push_*); at draw time,
// tl_cell answers in the same currency -- sprite ids -- so drawing is a
// table lookup with no policy. Assets stay factored: ground paintings, mask
// shapes, overlay art; never baked combinations.

#define MAP_SPRITE_CAP 1024

#define MAP_SITE_VARIANT_CAP 8

typedef struct {
  D_Sprite sprites[MAP_SPRITE_CAP]; // id 0 reserved: "no art"
  U32 sprite_count;
  TL_Config tiling;
  V4 fallback_colors[TL_CLASS_CAP]; // flat color for terrains with no ground art
  U32 class_count;
  // GM_Sprite -> sprite ids, one per art variant; count 0 = no art yet
  U32 gm_sprites[GM_Sprite_COUNT][MAP_SITE_VARIANT_CAP];
  U32 gm_sprite_counts[GM_Sprite_COUNT];
} MapAssets;

internal U32 map__register(MapAssets* assets, D_Sprite sprite) {
  U32 result = 0;
  if(assets->sprite_count < MAP_SPRITE_CAP) {
    assets->sprites[assets->sprite_count] = sprite;
    result = assets->sprite_count;
    assets->sprite_count += 1;
  }
  return result;
}

// one tile png -> sprite id; 0 = no such file, which is also the loader's
// "stop scanning variants" signal
internal U32 map__load(MapAssets* assets, Arena* scratch, String8 path) {
  U32 result = 0;
  D_Image image = d_image_load(scratch, path);
  if(image.w != 0) {
    result = map__register(assets, d_spritesheet_push(image));
  }
  return result;
}

// read-through cache around the tiler: a hit returns as-is, a miss asks the
// tiler and keeps the answer. tl_cell always emits at least one piece, so a
// zero cell reads as "not yet".
typedef struct {
  TL_Config tiling;
  TL_Cell* cells; // (width+1) x (height+1): the ring owns far-edge dual cells
  I32 w;
  I32 h;
} MapCache;

internal MapCache map_cache_make(Arena* arena, I32 board_w, I32 board_h, TL_Config tiling) {
  MapCache cache = {0};
  cache.tiling = tiling;
  cache.w = board_w + 1;
  cache.h = board_h + 1;
  cache.cells = push_array(arena, TL_Cell, (U64)cache.w * (U64)cache.h);
  return cache;
}

internal TL_Cell* map_cache_cell(MapCache* cache, GM_MapItem* item) {
  TL_Cell* cell = &cache->cells[item->pos.y * cache->w + item->pos.x];
  if(cell->count == 0) {
    U32 nb[9];
    for(U32 i = 0; i < 9; i += 1) { nb[i] = item->neighbours[i]; }
    U8 networks[TL_NETWORK_CAP] = {0};
    for(BD_Feature feature = 0; feature < BD_Feature_COUNT; feature += 1) {
      networks[feature] = item->features[feature];
    }
    *cell = tl_cell(&cache->tiling, nb, networks, item->pos);
  }
  return cell;
}

internal MapAssets map_assets_load(void) {
  MapAssets assets = {0};
  assets.sprite_count = 1; // id 0 = nil
  assets.class_count = WG_TERRAIN_TYPE_COUNT;
  ArenaTemp scratch = arena_get_scratch(0, 0);
  d_spritesheet_begin(512, 512, D_Sampling_Smooth);

  for(U32 type = 0; type < WG_TERRAIN_TYPE_COUNT; type += 1) {
    WG_TerrainType* def = &WG_TERRAIN_TYPES[type];
    String8 name = wg_terrain_name(type);
    assets.fallback_colors[type] = def->color;
    tl_class_set(&assets.tiling, type, def->rank, def->overlay_density);

    // ground: one torus painting per terrain, cut into 4x4 windows on the
    // OFFSET grid -- each half a tile past the painting's own grid, since
    // the tiler draws ground on dual cells (see tiling.h). The wrap margin
    // keeps every offset window one contiguous crop; push_window fills each
    // window's atlas gutter with its true neighbor texels, so windows butt
    // seamlessly on screen.
    String8 ground_path = push_str8f(scratch.arena, "assets/tiles/%S_ground.png", name);
    D_Image torus = d_image_load(scratch.arena, ground_path);
    if(torus.w > 0) {
      I32 tw = torus.w / TL_TORUS_GRID;
      I32 th = torus.h / TL_TORUS_GRID;
      D_Image ext = d_image_create(scratch.arena, torus.w + tw, torus.h + th, (V4){0});
      d_image_blit(ext, 0, 0, torus);
      d_image_blit(ext, torus.w, 0, torus); // wrap margins; blit clips
      d_image_blit(ext, 0, torus.h, torus);
      d_image_blit(ext, torus.w, torus.h, torus);
      for(U32 gy = 0; gy < TL_TORUS_GRID; gy += 1) {
        for(U32 gx = 0; gx < TL_TORUS_GRID; gx += 1) {
          I32 wx0 = (I32)gx * tw + tw / 2;
          I32 wy0 = (I32)gy * th + th / 2;
          Rect window = {{(F32)wx0, (F32)wy0}, {(F32)(wx0 + tw), (F32)(wy0 + th)}};
          tl_push_ground(&assets.tiling, type, gx, gy,
                         map__register(&assets, d_spritesheet_push_window(ext, window)));
        }
      }
    }

    for(U32 variant = 0;; variant += 1) {
      String8 path = push_str8f(scratch.arena, "assets/tiles/%S_overlay_%u.png", name, variant);
      U32 id = map__load(&assets, scratch.arena, path);
      if(id == 0) { break; }
      tl_push_overlay(&assets.tiling, type, id);
    }
    for(U32 variant = 0;; variant += 1) {
      String8 path = push_str8f(scratch.arena, "assets/tiles/%S_edge_%u.png", name, variant);
      U32 id = map__load(&assets, scratch.arena, path);
      if(id == 0) { break; }
      tl_push_edge_overlay(&assets.tiling, type, id);
    }
  }

  // boundary masks are class-agnostic: cases 1..14 (0 and 15 are the
  // trivial empty/full cases and have no files)
  for(U32 code = 1; code < 15; code += 1) {
    for(U32 variant = 0;; variant += 1) {
      String8 path = push_str8f(scratch.arena, "assets/tiles/mask_%u_%u.png", code, variant);
      U32 id = map__load(&assets, scratch.arena, path);
      if(id == 0) { break; }
      tl_push_mask(&assets.tiling, code, id);
    }
  }

  // network art, by BD_Feature id; finished pieces per connection case
  struct {
    U32 network;
    char* name;
  } NETWORK_ART[] = {
      {BD_Feature_River, "river"},
      {BD_Feature_Road, "road"},
  };
  for(U32 i = 0; i < ArrayCount(NETWORK_ART); i += 1) {
    for(U32 code = 1; code < 16; code += 1) {
      for(U32 variant = 0;; variant += 1) {
        String8 path = push_str8f(scratch.arena, "assets/tiles/%s_%u_%u.png",
                                  NETWORK_ART[i].name, code, variant);
        U32 id = map__load(&assets, scratch.arena, path);
        if(id == 0) { break; }
        tl_push_network(&assets.tiling, NETWORK_ART[i].network, code, id);
      }
    }
  }

  // item art, by GM_Sprite id -- the game names what a thing looks like,
  // these files say how that looks. Variant-scanned like all tile art; a
  // sprite with no files keeps count 0 and the renderer falls back to a
  // flat shape.
  struct {
    GM_Sprite sprite;
    char* name;
  } ITEM_ART[] = {
      {GM_Sprite_Village, "village"},
      {GM_Sprite_Palace, "palace"},
      {GM_Sprite_Herders, "herders"},
      {GM_Sprite_Wagon, "wagon"},
      {GM_Sprite_Tholos, "tholos"},
      {GM_Sprite_Band, "band"},
  };
  for(U32 i = 0; i < ArrayCount(ITEM_ART); i += 1) {
    for(U32 variant = 0; variant < MAP_SITE_VARIANT_CAP; variant += 1) {
      String8 path = push_str8f(scratch.arena, "assets/tiles/site_%s_%u.png", ITEM_ART[i].name, variant);
      U32 id = map__load(&assets, scratch.arena, path);
      if(id == 0) { break; }
      assets.gm_sprites[ITEM_ART[i].sprite][variant] = id;
      assets.gm_sprite_counts[ITEM_ART[i].sprite] = variant + 1;
    }
  }

  d_spritesheet_end();
  arena_release_scratch(scratch);
  return assets;
}


// draw order: the tiler's layers first (a fact about how the art is built --
// boundary tongues must not paint over overlay art), then surface items
#define MAP_BIN_COUNT (TL_Layer_COUNT + 1)

// the world-space rect of the tile at (x, y) -- fractional positions land on
// the dual grid -- shrunk by `inset` world units per side
internal Rect map_tile_rect(F32 x, F32 y, F32 inset) {
  return (Rect){{x * MAP_TILE + inset, y * MAP_TILE + inset},
                {(x + 1) * MAP_TILE - inset, (y + 1) * MAP_TILE - inset}};
}

internal void map_draw(GM_MapItems items, MapAssets* assets, MapCache* cache, D_Camera camera) {
  // One resolved drawable, ready to submit. sprite 0 means "draw color".
  typedef struct {
    Rect rect;
    U32 sprite; // into assets->sprites; 0 = none
    U32 mask;   // sprite mask, 0 = none
    V4 color;   // sprite tint (zero = as-is), or the fill when sprite == 0
    F32 rounding;
  } MapDrawCmd;

  V2 vp = wnd_size();
  d_rect((Rect){{0, 0}, {vp.x, vp.y}}, (V4){0.06f, 0.06f, 0.08f, 1});

  ArenaTemp scratch = arena_get_scratch(0, 0);

  //- size the bins: one slot per piece / surface item. Upper bounds -- pieces
  // that resolve to nothing leave slack. This pass also warms the cell cache,
  // so the scatter below re-resolves for free.
  U64 cap[MAP_BIN_COUNT] = {0};
  for(U64 i = 0; i < items.count; i += 1) {
    GM_MapItem* item = &items.items[i];
    if(item->has_pawn) {
      cap[TL_Layer_COUNT] += 1;
      continue;
    }
    TL_Cell* cell = map_cache_cell(cache, item);
    for(U32 pi = 0; pi < cell->count; pi += 1) { cap[cell->pieces[pi].layer] += 1; }
  }
  U64 base[MAP_BIN_COUNT];
  U64 total = 0;
  for(U32 bin = 0; bin < MAP_BIN_COUNT; bin += 1) {
    base[bin] = total;
    total += cap[bin];
  }
  MapDrawCmd* cmds = push_array_no_zero(scratch.arena, MapDrawCmd, total);
  U64 count[MAP_BIN_COUNT] = {0};

  //- scatter: resolve each item once, every drawable lands in its bin's slots
  for(U64 i = 0; i < items.count; i += 1) {
    GM_MapItem* item = &items.items[i];
    if(item->has_pawn) {
      MapDrawCmd* cmd = &cmds[base[TL_Layer_COUNT] + count[TL_Layer_COUNT]];
      count[TL_Layer_COUNT] += 1;
      U32 variant_count = assets->gm_sprite_counts[item->sprite];
      if(variant_count > 0) {
        // full tile; the art brings its own silhouette, the item's color
        // rides along as tint (white = as-is). Variant hashed off the thing
        // id: stable for the thing's whole life, varied across things.
        U32 variant = (item->id * 2654435761u) % variant_count;
        *cmd = (MapDrawCmd){
            .rect = map_tile_rect(item->pos.x, item->pos.y, 0),
            .sprite = assets->gm_sprites[item->sprite][variant],
            .color = item->color,
        };
      } else {
        F32 inset = MAP_TILE * 0.2f;
        *cmd = (MapDrawCmd){
            .rect = map_tile_rect(item->pos.x, item->pos.y, inset),
            .color = item->color,
            .rounding = (MAP_TILE - 2 * inset) * 0.35f,
        };
      }
      continue;
    }
    TL_Cell* cell = map_cache_cell(cache, item);
    for(U32 pi = 0; pi < cell->count; pi += 1) {
      TL_Piece* piece = &cell->pieces[pi];
      Rect r = map_tile_rect(item->pos.x + piece->offset.x, item->pos.y + piece->offset.y, 0);
      MapDrawCmd* cmd = &cmds[base[piece->layer] + count[piece->layer]];
      if(piece->id != 0) {
        count[piece->layer] += 1;
        *cmd = (MapDrawCmd){.rect = r, .sprite = piece->id, .mask = piece->mask_id};
      } else if(piece->klass != 0 && item->has_terrain) {
        // artless terrain draws its flat color; nil (class 0) is the void
        // and draws nothing
        count[piece->layer] += 1;
        *cmd = (MapDrawCmd){.rect = r, .color = assets->fallback_colors[piece->klass]};
      }
    }
  }

  //- submit: bins in order, commands in scatter order
  d_camera_begin(camera);
  for(U32 bin = 0; bin < MAP_BIN_COUNT; bin += 1) {
    for(U64 i = 0; i < count[bin]; i += 1) {
      MapDrawCmd* cmd = &cmds[base[bin] + i];
      if(cmd->sprite != 0) {
        if(cmd->mask != 0) {
          d_sprite_masked(assets->sprites[cmd->sprite], assets->sprites[cmd->mask], cmd->rect, cmd->color);
        } else {
          d_sprite(assets->sprites[cmd->sprite], cmd->rect, cmd->color);
        }
      } else if(cmd->rounding > 0) {
        d_rect_rounded(cmd->rect, cmd->color, cmd->rounding);
      } else {
        d_rect(cmd->rect, cmd->color);
      }
    }
  }
  d_camera_end();
  arena_release_scratch(scratch);
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
//~ fp: Entry Point

////////////////////////////////
//~ fp: temp: Worldgen Report
//
// `app worlds N [first_seed]`: generate N worlds headless and print one CSV
// row of metrics per world to stdout, for offline analysis.

typedef struct {
  U16 group;
  U16 touches_border;
  I32 size;
} Report_Component;

// 4-connected components of equal group values; group 0 tiles are skipped
internal I32 report__components(Arena* arena, U16* groups, I32 w, I32 h,
                                Report_Component* out) {
  U8* visited = push_array(arena, U8, (U64)w * h);
  V2I* queue = push_array_no_zero(arena, V2I, (U64)w * h);
  I32 count = 0;
  for(I32 y = 0; y < h; y += 1) {
    for(I32 x = 0; x < w; x += 1) {
      U64 idx = (U64)y * w + x;
      if(visited[idx] || groups[idx] == 0) { continue; }
      Report_Component comp = {groups[idx], 0, 0};
      visited[idx] = 1;
      queue[0] = (V2I){x, y};
      I32 queue_count = 1;
      for(I32 head = 0; head < queue_count; head += 1) {
        V2I p = queue[head];
        comp.size += 1;
        if(p.x == 0 || p.y == 0 || p.x == w - 1 || p.y == h - 1) { comp.touches_border = 1; }
        for(Dir4 dir = 0; dir < Dir4_COUNT; dir += 1) {
          V2I n = v2i_add(p, dir4_delta(dir));
          if(n.x < 0 || n.y < 0 || n.x >= w || n.y >= h) { continue; }
          U64 nidx = (U64)n.y * w + n.x;
          if(visited[nidx] || groups[nidx] != comp.group) { continue; }
          visited[nidx] = 1;
          queue[queue_count] = n;
          queue_count += 1;
        }
      }
      out[count] = comp;
      count += 1;
    }
  }
  return count;
}

internal void worldgen_report(I32 world_count, U64 first_seed) {
  WG_Params params = wg_params_load(str8_lit("data/world.tabula"));

  // the report knows some terrains by role; ids resolve by name so the file
  // stays free to reorder
  U32 id_water = wg_terrain_by_name(str8_lit("water"));
  U32 id_ocean = wg_terrain_by_name(str8_lit("ocean"));
  U32 id_ice = wg_terrain_by_name(str8_lit("ice"));
  U32 id_beach = wg_terrain_by_name(str8_lit("beach"));
  U32 id_mountain = wg_terrain_by_name(str8_lit("mountain"));
  U32 id_snowcap = wg_terrain_by_name(str8_lit("snowcap"));
  B32 is_hot[WG_TERRAIN_CAP] = {0};
  B32 is_cold[WG_TERRAIN_CAP] = {0};
  is_hot[wg_terrain_by_name(str8_lit("desert"))] = 1;
  is_hot[wg_terrain_by_name(str8_lit("badlands"))] = 1;
  is_hot[wg_terrain_by_name(str8_lit("savanna"))] = 1;
  is_hot[wg_terrain_by_name(str8_lit("jungle"))] = 1;
  is_cold[id_ice] = 1;
  is_cold[id_snowcap] = 1;
  is_cold[wg_terrain_by_name(str8_lit("tundra"))] = 1;
  is_cold[wg_terrain_by_name(str8_lit("taiga"))] = 1;

  printf_str8("seed");
  for(U32 i = 1; i < params.terrain_count; i += 1) { printf_str8(",%S", wg_terrain_name(i)); }
  printf_str8(",land,landmasses,largest_landmass,ranges,largest_range,specks,lakes"
              ",river_tiles,river_nets,coast,beach_on_coast,hot_cold,nil,passable_largest\n");

  Arena* world_arena = arena_alloc();
  for(I32 world = 0; world < world_count; world += 1) {
    arena_clear(world_arena);
    U64 seed = first_seed + (U64)world;
    TH_Db* db = th_init_db(world_arena);
    wg_generate(db, &params, seed);
    I32 w = params.width;
    I32 h = params.height;
    U64 tiles = (U64)w * h;

    U32 counts[WG_TERRAIN_CAP] = {0};
    U16* by_terrain = push_array_no_zero(world_arena, U16, tiles); // terrain+1: no group 0
    U16* by_land = push_array(world_arena, U16, tiles);
    U16* by_range = push_array(world_arena, U16, tiles);
    U16* by_passable = push_array(world_arena, U16, tiles);
    U16* by_river = push_array(world_arena, U16, tiles);
    U64 river_tiles = 0;
    U64 coast = 0;
    U64 hot_cold = 0;
    for(I32 y = 0; y < h; y += 1) {
      for(I32 x = 0; x < w; x += 1) {
        U64 idx = (U64)y * w + x;
        V2I p = {x, y};
        U32 t = (U32)th_ifield_get(db, p, TH_IField_Terrain);
        counts[t] += 1;
        B32 land = t != 0 && t != id_water && t != id_ocean && t != id_ice;
        by_terrain[idx] = (U16)(t + 1);
        by_land[idx] = (U16)land;
        by_range[idx] = t == id_mountain || t == id_snowcap;
        by_passable[idx] = t < WG_TERRAIN_TYPE_COUNT && WG_TERRAIN_TYPES[t].move_cost > 0;
        by_river[idx] = th_ifield_get(db, p, TH_IField_RiverMask) != 0;
        river_tiles += by_river[idx];
        if(land) {
          B32 sea_beside = 0;
          for(Dir4 dir = 0; dir < Dir4_COUNT; dir += 1) {
            V2I n = v2i_add(p, dir4_delta(dir));
            U32 nt = (U32)th_ifield_get(db, n, TH_IField_Terrain);
            sea_beside |= th_world_in_bounds(db, n) && (nt == id_water || nt == id_ocean);
          }
          coast += sea_beside;
        }
        // unordered adjacent pairs: only look right and down
        for(U32 pair = 0; pair < 2; pair += 1) {
          V2I n = pair == 0 ? (V2I){x + 1, y} : (V2I){x, y + 1};
          if(!th_world_in_bounds(db, n)) { continue; }
          U32 nt = (U32)th_ifield_get(db, n, TH_IField_Terrain);
          hot_cold += (is_hot[t] && is_cold[nt]) || (is_cold[t] && is_hot[nt]);
        }
      }
    }

    Report_Component* comps = push_array_no_zero(world_arena, Report_Component, tiles);
    I32 speck_count = 0;
    I32 lake_count = 0;
    I32 comp_count = report__components(world_arena, by_terrain, w, h, comps);
    for(I32 i = 0; i < comp_count; i += 1) {
      U32 t = (U32)comps[i].group - 1;
      speck_count += comps[i].size <= 2;
      lake_count += (t == id_water || t == id_ocean) && !comps[i].touches_border;
    }
    I32 landmass_count = report__components(world_arena, by_land, w, h, comps);
    I32 largest_landmass = 0;
    U64 land_tiles = 0;
    for(I32 i = 0; i < landmass_count; i += 1) {
      largest_landmass = Max(largest_landmass, comps[i].size);
      land_tiles += (U64)comps[i].size;
    }
    I32 range_count = report__components(world_arena, by_range, w, h, comps);
    I32 largest_range = 0;
    for(I32 i = 0; i < range_count; i += 1) { largest_range = Max(largest_range, comps[i].size); }
    I32 passable_count = report__components(world_arena, by_passable, w, h, comps);
    U64 passable_tiles = 0;
    I32 largest_passable = 0;
    for(I32 i = 0; i < passable_count; i += 1) {
      largest_passable = Max(largest_passable, comps[i].size);
      passable_tiles += (U64)comps[i].size;
    }
    I32 river_net_count = report__components(world_arena, by_river, w, h, comps);

    printf_str8("%llu", (unsigned long long)seed);
    for(U32 i = 1; i < params.terrain_count; i += 1) {
      printf_str8(",%.4f", (F32)counts[i] / (F32)tiles);
    }
    printf_str8(",%.4f,%d,%.4f,%d,%d,%d,%d,%llu,%d,%llu,%.4f,%llu,%u,%.4f\n",
                (F32)land_tiles / (F32)tiles,
                landmass_count,
                land_tiles > 0 ? (F32)largest_landmass / (F32)land_tiles : 0.0f,
                range_count,
                largest_range,
                speck_count,
                lake_count,
                (unsigned long long)river_tiles,
                river_net_count,
                (unsigned long long)coast,
                coast > 0 ? (F32)counts[id_beach] / (F32)coast : 0.0f,
                (unsigned long long)hot_cold,
                counts[0],
                passable_tiles > 0 ? (F32)largest_passable / (F32)passable_tiles : 0.0f);
  }
}

int main(int argc, char** argv) {
  TCTX tctx;
  tctx_init_and_equip(&tctx);

  //- fp: temp: headless report mode exits before any window exists
  if(argc >= 3 && str8_match(str8_cstring(argv[1]), str8_lit("worlds"), 0)) {
    I32 world_count = 0;
    String8 count_arg = str8_cstring(argv[2]);
    for(U64 i = 0; i < count_arg.size; i += 1) { world_count = world_count * 10 + (count_arg.str[i] - '0'); }
    U64 first_seed = 1;
    if(argc >= 4) {
      first_seed = 0;
      String8 seed_arg = str8_cstring(argv[3]);
      for(U64 i = 0; i < seed_arg.size; i += 1) { first_seed = first_seed * 10 + (U64)(seed_arg.str[i] - '0'); }
    }
    worldgen_report(world_count, first_seed);
    tctx_release();
    return 0;
  }

  Arena* frame_arena = arena_alloc();
  wnd_open(str8_lit("Imperium"), 1600, 900);
  wnd_equip_gl();
  d_init();

  //- fp: temp: the long-lived game state, and the presentation state that
  //  shadows it (rebuilt with it on every reseed)
  Arena* game_arena = arena_alloc();
  GM_Game game = {0};
  MapCache map_cache = {0};
  U64 game_next_seed = 2704;

  MapAssets map_assets = {0};
  {
    // the terrain type table drives asset naming and tiling registration;
    // loading the params fills it
    wg_params_load(str8_lit("data/world.tabula"));
    map_assets = map_assets_load();
  }

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
      gm_init(game_arena, &game, game_next_seed++);
      map_cache = map_cache_make(game_arena, game.board->width, game.board->height, map_assets.tiling);
      // Reset the camera
      camera.center = (V2){game.board->width * MAP_TILE / 2, game.board->height * MAP_TILE / 2};
      camera.zoom = 1.0f; // whole map in view; scroll to zoom in
    }

    gm_update(&game, wnd_frame_time());

    d_frame_begin(frame_arena, wnd_size_px(), wnd_scale());
    {
      // visible tile window, one ring past the right/bottom edge: each cell
      // owns the dual boundary cell at its NW corner, so the far-edge duals
      // need an owner. Board dims read off the cache (cache is board+1).
      V2 world_min = d_camera_from_screen(camera, (V2){0, 0});
      V2 world_max = d_camera_from_screen(camera, wnd_size());
      V2I view_min = {ClampBot((I32)(world_min.x / MAP_TILE) - 1, 0),
                      ClampBot((I32)(world_min.y / MAP_TILE) - 1, 0)};
      V2I view_max = {ClampTop((I32)(world_max.x / MAP_TILE) + 1, map_cache.w - 2) + 1,
                      ClampTop((I32)(world_max.y / MAP_TILE) + 1, map_cache.h - 2) + 1};
      GM_MapItems map_items = gm_map_items(frame_arena, &game, view_min, view_max);
      map_draw(map_items, &map_assets, &map_cache, camera); // fp: parked for the pacing experiment below
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
#include "game/tiling.c"
#include "game/game.c"
#include "gfx/gfx.c"

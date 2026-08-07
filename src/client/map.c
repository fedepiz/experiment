#include "base/core.h"
#include "base/math.h"
#include "base/arena.h"
#include "base/strings.h"
#include "base/tctx.h"
#include "game/worldgen.h"
#include "game/tiling.h"
#include "game/game.h"
#include "gfx/window.h"
#include "gfx/draw.h"
#include "ui.h"
#include "client/map.h"

#define MAP_SPRITE_CAP       1024
#define MAP_PAWN_VARIANT_CAP 8

struct Map_View {
  //- assets: one flat sprite table plus the tiling registration built while
  //  filling it
  D_Sprite sprites[MAP_SPRITE_CAP]; // id 0 reserved: "no art"
  U32 sprite_count;
  TL_Config tiling;
  // pawn art: GM_Sprite -> sprite ids, one per variant; count 0 = no art yet
  U32 gm_sprites[GM_Sprite_COUNT][MAP_PAWN_VARIANT_CAP];
  U32 gm_sprite_counts[GM_Sprite_COUNT];

  //- read-through cell cache around the tiler: a hit returns as-is, a miss
  //  asks the tiler and keeps the answer. tl_cell always emits at least one
  //  piece, so a zero cell reads as "not yet".
  TL_Cell* cells; // (width+1) x (height+1): the ring owns far-edge dual cells
  I32 cache_w;
  I32 cache_h;
};

////////////////////////////////
//~ fp: Map Assets
//
// The loader narrates what exists to the tiler (tl_push_*); at draw time,
// tl_cell answers in the same currency -- sprite ids -- so drawing is a
// table lookup with no policy. Assets stay factored: ground paintings, mask
// shapes, overlay art; never baked combinations.

internal U32 map__register(Map_View* map, D_Sprite sprite) {
  U32 result = 0;
  if(map->sprite_count < MAP_SPRITE_CAP) {
    map->sprites[map->sprite_count] = sprite;
    result = map->sprite_count;
    map->sprite_count += 1;
  }
  return result;
}

// one tile png -> sprite id; 0 = no such file, which is also the loader's
// "stop scanning variants" signal
internal U32 map__load(Map_View* map, Arena* scratch, String8 path) {
  U32 result = 0;
  D_Image image = d_image_load(scratch, path);
  if(image.w != 0) {
    result = map__register(map, d_spritesheet_push(image));
  }
  return result;
}

internal void map_world_changed(Map_View* map, Arena* arena, I32 board_w, I32 board_h) {
  map->cache_w = board_w + 1;
  map->cache_h = board_h + 1;
  map->cells = push_array(arena, TL_Cell, (U64)map->cache_w * (U64)map->cache_h);
}

internal TL_Cell* map__cell(Map_View* map, GM_MapItem* item) {
  TL_Cell* cell = &map->cells[item->pos.y * map->cache_w + item->pos.x];
  if(cell->count == 0) {
    U32 nb[9];
    for(U32 i = 0; i < 9; i += 1) { nb[i] = item->neighbours[i]; }
    U8 networks[TL_NETWORK_CAP] = {0};
    for(BD_Feature feature = 0; feature < BD_Feature_COUNT; feature += 1) {
      networks[feature] = item->features[feature];
    }
    *cell = tl_cell(&map->tiling, nb, networks, item->pos);
  }
  return cell;
}

internal Map_View* map_init(Arena* arena) {
  Map_View* map = push_array(arena, Map_View, 1);
  map->sprite_count = 1; // id 0 = nil
  ArenaTemp scratch = arena_get_scratch(0, 0);
  d_spritesheet_begin(512, 512, D_Sampling_Smooth);

  for(U32 type = 0; type < WG_TERRAIN_TYPE_COUNT; type += 1) {
    WG_TerrainType* def = &WG_TERRAIN_TYPES[type];
    String8 name = wg_terrain_name(type);
    tl_class_set(&map->tiling, type, def->rank, def->overlay_density);

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
          tl_push_ground(&map->tiling, type, gx, gy,
                         map__register(map, d_spritesheet_push_window(ext, window)));
        }
      }
    }

    for(U32 variant = 0;; variant += 1) {
      String8 path = push_str8f(scratch.arena, "assets/tiles/%S_overlay_%u.png", name, variant);
      U32 id = map__load(map, scratch.arena, path);
      if(id == 0) { break; }
      tl_push_overlay(&map->tiling, type, id);
    }
    for(U32 variant = 0;; variant += 1) {
      String8 path = push_str8f(scratch.arena, "assets/tiles/%S_edge_%u.png", name, variant);
      U32 id = map__load(map, scratch.arena, path);
      if(id == 0) { break; }
      tl_push_edge_overlay(&map->tiling, type, id);
    }
  }

  // boundary masks are class-agnostic: cases 1..14 (0 and 15 are the
  // trivial empty/full cases and have no files)
  for(U32 code = 1; code < 15; code += 1) {
    for(U32 variant = 0;; variant += 1) {
      String8 path = push_str8f(scratch.arena, "assets/tiles/mask_%u_%u.png", code, variant);
      U32 id = map__load(map, scratch.arena, path);
      if(id == 0) { break; }
      tl_push_mask(&map->tiling, code, id);
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
        U32 id = map__load(map, scratch.arena, path);
        if(id == 0) { break; }
        tl_push_network(&map->tiling, NETWORK_ART[i].network, code, id);
      }
    }
  }

  // pawn art, by GM_Sprite id -- the game names what a thing looks like,
  // these files say how that looks. Variant-scanned like all tile art; a
  // sprite with no files keeps count 0 and the renderer falls back to a
  // flat shape.
  struct {
    GM_Sprite sprite;
    char* name;
  } PAWN_ART[] = {
      {GM_Sprite_Village, "village"},
      {GM_Sprite_Palace, "palace"},
      {GM_Sprite_Herders, "herders"},
      {GM_Sprite_Wagon, "wagon"},
      {GM_Sprite_Tholos, "tholos"},
      {GM_Sprite_Band, "band"},
  };
  for(U32 i = 0; i < ArrayCount(PAWN_ART); i += 1) {
    for(U32 variant = 0; variant < MAP_PAWN_VARIANT_CAP; variant += 1) {
      String8 path = push_str8f(scratch.arena, "assets/tiles/site_%s_%u.png", PAWN_ART[i].name, variant);
      U32 id = map__load(map, scratch.arena, path);
      if(id == 0) { break; }
      map->gm_sprites[PAWN_ART[i].sprite][variant] = id;
      map->gm_sprite_counts[PAWN_ART[i].sprite] = variant + 1;
    }
  }

  d_spritesheet_end();
  arena_release_scratch(scratch);
  return map;
}


// draw order: the tiler's layers first (a fact about how the art is built --
// boundary tongues must not paint over overlay art), then surface items,
// then highlights above everything
#define MAP_LAYER_SURFACE   TL_Layer_COUNT
#define MAP_LAYER_HIGHLIGHT (TL_Layer_COUNT + 1)
#define MAP_LAYER_COUNT     (TL_Layer_COUNT + 2)

// the world-space rect of the tile at (x, y) -- fractional positions land on
// the dual grid -- shrunk by `inset` world units per side
internal Rect map_tile_rect(F32 x, F32 y, F32 inset) {
  return (Rect){{x * MAP_TILE + inset, y * MAP_TILE + inset},
                {(x + 1) * MAP_TILE - inset, (y + 1) * MAP_TILE - inset}};
}

// the tile cell under a screen position, taken in client points as
// wnd_mouse_pos gives them. Floors, so cells west/north of the board come
// back negative -- callers bounds-check against the board.
internal V2I map_tile_from_screen(D_Camera camera, V2 screen) {
  V2 world = d_camera_from_screen(camera, screen);
  V2I tile = {(I32)(world.x / MAP_TILE), (I32)(world.y / MAP_TILE)};
  // (I32) truncates toward zero; subtracting the comparison corrects that
  // to a true floor for negative coordinates
  tile.x -= (F32)tile.x * MAP_TILE > world.x;
  tile.y -= (F32)tile.y * MAP_TILE > world.y;
  return tile;
}

internal void map_draw(Map_View* map, GM_MapItems items, D_Camera camera) {
  // One resolved drawable, ready to submit. sprite 0 means "draw color".
  typedef struct {
    Rect rect;
    U32 sprite; // into map->sprites; 0 = none
    U32 mask;   // sprite mask, 0 = none
    V4 color;   // sprite tint (zero = as-is), or the fill when sprite == 0
    F32 rounding;
    F32 outline; // > 0: outline of this thickness (world units) instead of a fill
  } MapDrawCmd;

  V2 vp = wnd_size();
  d_rect((Rect){{0, 0}, {vp.x, vp.y}}, (V4){0.06f, 0.06f, 0.08f, 1});

  ArenaTemp scratch = arena_get_scratch(0, 0);

  //- size the layers: one slot per piece / surface item. Upper bounds --
  // pieces that resolve to nothing leave slack. This pass also warms the cell
  // cache, so the scatter below re-resolves for free.
  U64 cap[MAP_LAYER_COUNT] = {0};
  for(U64 i = 0; i < items.count; i += 1) {
    GM_MapItem* item = &items.items[i];
    if(item->has_highlight) {
      cap[MAP_LAYER_HIGHLIGHT] += 1;
      continue;
    }
    if(item->has_pawn) {
      cap[MAP_LAYER_SURFACE] += 1;
      continue;
    }
    TL_Cell* cell = map__cell(map, item);
    for(U32 pi = 0; pi < cell->count; pi += 1) { cap[cell->pieces[pi].layer] += 1; }
  }
  U64 base[MAP_LAYER_COUNT];
  U64 total = 0;
  for(U32 layer = 0; layer < MAP_LAYER_COUNT; layer += 1) {
    base[layer] = total;
    total += cap[layer];
  }
  MapDrawCmd* cmds = push_array_no_zero(scratch.arena, MapDrawCmd, total);
  U64 count[MAP_LAYER_COUNT] = {0};

  //- scatter: resolve each item once, every drawable lands in its layer's slots
  for(U64 i = 0; i < items.count; i += 1) {
    GM_MapItem* item = &items.items[i];
    if(item->has_highlight) {
      MapDrawCmd* cmd = &cmds[base[MAP_LAYER_HIGHLIGHT] + count[MAP_LAYER_HIGHLIGHT]];
      count[MAP_LAYER_HIGHLIGHT] += 1;
      *cmd = (MapDrawCmd){
          .rect = map_tile_rect(item->pos.x, item->pos.y, 0),
          .color = ui_color_from_name(str8_lit("accent")),
          .outline = 1.0f,
      };
      continue;
    }
    if(!item->has_pawn) { // ground cells, off-board ring included -- mirrors the sizing pass
      TL_Cell* cell = map__cell(map, item);
      for(U32 pi = 0; pi < cell->count; pi += 1) {
        TL_Piece* piece = &cell->pieces[pi];

        V4 color = {0};
        if(piece->id != 0) {
          color = item->color;
        }

        Rect r = map_tile_rect(item->pos.x + piece->offset.x, item->pos.y + piece->offset.y, 0);
        MapDrawCmd* cmd = &cmds[base[piece->layer] + count[piece->layer]];
        *cmd = (MapDrawCmd){.rect = r, .sprite = piece->id, .color = color, .mask = piece->mask_id};
        count[piece->layer] += 1;
      }
    }

    if(item->has_pawn) {
      U32 variant_count = map->gm_sprite_counts[item->sprite];

      F32 inset = 0.0;
      F32 rounding = 0.0;
      U32 sprite = 0;

      if(variant_count > 0) {
        // full tile; the art brings its own silhouette, the item's color
        // rides along as tint (white = as-is). Variant hashed off the thing
        // id: stable for the thing's whole life, varied across things.
        U32 variant = (item->id * 2654435761u) % variant_count;
        sprite = map->gm_sprites[item->sprite][variant];
      } else {
        inset = MAP_TILE * 0.2f;
        rounding = (MAP_TILE - 2 * inset) * 0.35f;
      }

      MapDrawCmd* cmd = &cmds[base[MAP_LAYER_SURFACE] + count[MAP_LAYER_SURFACE]];
      count[MAP_LAYER_SURFACE] += 1;
      *cmd = (MapDrawCmd){
          .rect = map_tile_rect(item->pos.x, item->pos.y, inset),
          .sprite = sprite,
          .rounding = rounding,
          .color = item->color,
      };
    }
  }

  //- submit: layers in order, commands in scatter order
  d_camera_begin(camera);
  for(U32 layer = 0; layer < MAP_LAYER_COUNT; layer += 1) {
    for(U64 i = 0; i < count[layer]; i += 1) {
      MapDrawCmd* cmd = &cmds[base[layer] + i];
      if(cmd->sprite != 0) {
        if(cmd->mask != 0) {
          d_sprite_masked(map->sprites[cmd->sprite], map->sprites[cmd->mask], cmd->rect, cmd->color);
        } else {
          d_sprite(map->sprites[cmd->sprite], cmd->rect, cmd->color);
        }
      } else if(cmd->outline > 0) {
        d_rect_outline(cmd->rect, cmd->color, cmd->outline);
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

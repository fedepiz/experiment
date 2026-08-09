#include "base/core.h"
#include "base/math.h"
#include "base/rng.h"
#include "base/arena.h"
#include "base/strings.h"
#include "base/tctx.h"
#include "game/worldgen.h"
#include "game/tiling.h"
#include "game/game.h"
#include "gfx/color.h"
#include "gfx/window.h"
#include "gfx/draw.h"
#include "ui.h"
#include "client/map.h"

#define MAP_SPRITE_CAP       1024
#define MAP_PAWN_VARIANT_CAP 8

struct Map_View {
  //- The assets: one table of sprites, and the registration in the tiler that
  //  the loader makes while it fills that table.
  D_Sprite sprites[MAP_SPRITE_CAP]; // The id 0 says that there is no art.
  U32 sprite_count;
  TL_Config tiling;
  // The art of a pawn. Each GM_Sprite gives a list of sprite ids, one for each
  // variant. A count of 0 says that the sprite has no art.
  U32 gm_sprites[GM_Sprite_COUNT][MAP_PAWN_VARIANT_CAP];
  U32 gm_sprite_counts[GM_Sprite_COUNT];

  //- The cache of the cells of the tiler. An entry that exists comes back as
  //  it is. Where an entry is absent, this module calls the tiler and keeps the
  //  answer. tl_cell always gives one piece or more, so a cell of zeros says
  //  that the cache has no entry.
  TL_Cell* cells; // (width+1) by (height+1). The extra ring holds the dual cells of the far edges.
  I32 cache_w;
  I32 cache_h;
};

////////////////////////////////
//~ fp: Map Assets
//
// The loader tells the tiler what art exists, with the tl_push_ functions. At
// the draw, tl_cell answers with those same sprite ids, so the draw is a read
// of a table and holds no rule.
//
// The assets stay separate: a painting of the ground, a shape of a mask, and a
// piece of overlay art. No file holds a combination of two of those.

internal U32 map__register(Map_View* map, D_Sprite sprite) {
  U32 result = 0;
  if(map->sprite_count < MAP_SPRITE_CAP) {
    map->sprites[map->sprite_count] = sprite;
    result = map->sprite_count;
    map->sprite_count += 1;
  }
  return result;
}

// It reads one png of a tile and gives a sprite id. A result of 0 says that the
// file does not exist, which also tells the loader to stop its search for more
// variants.
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
  map->sprite_count = 1; // the id 0 is the nil sprite
  ArenaTemp scratch = arena_get_scratch(0, 0);
  d_spritesheet_begin(512, 512, D_Sampling_Smooth);

  for(U32 type = 0; type < WG_TERRAIN_TYPE_COUNT; type += 1) {
    WG_TerrainType* def = &WG_TERRAIN_TYPES[type];
    String8 name = wg_terrain_name(type);
    tl_class_set(&map->tiling, type, def->rank, def->overlay_density);

    // The ground. Each terrain has one painting that wraps on both axes. The
    // loader cuts that painting into 4x4 windows on the OFFSET grid, which is
    // half a tile past the grid of the painting itself, because the tiler
    // draws the ground on the dual cells. See tiling.h.
    //
    // The margin of the wrap keeps each window of that offset grid one
    // continuous part of the image. push_window fills the gutter of each
    // window in the sheet with the true texels next to that window, so two
    // windows meet with no seam on the screen.
    String8 ground_path = push_str8f(scratch.arena, "assets/tiles/%S_ground.png", name);
    D_Image torus = d_image_load(scratch.arena, ground_path);
    if(torus.w > 0) {
      I32 tw = torus.w / TL_TORUS_GRID;
      I32 th = torus.h / TL_TORUS_GRID;
      D_Image ext = d_image_create(scratch.arena, torus.w + tw, torus.h + th, (V4){0});
      d_image_blit(ext, 0, 0, torus);
      d_image_blit(ext, torus.w, 0, torus); // the margins of the wrap. The blit clips what goes past the image.
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

  // A mask of a boundary does not name a class of terrain. The cases are 1 to
  // 14. The case 0 is an empty cell and the case 15 is a full cell, and neither
  // has a file.
  for(U32 code = 1; code < 15; code += 1) {
    for(U32 variant = 0;; variant += 1) {
      String8 path = push_str8f(scratch.arena, "assets/tiles/mask_%u_%u.png", code, variant);
      U32 id = map__load(map, scratch.arena, path);
      if(id == 0) { break; }
      tl_push_mask(&map->tiling, code, id);
    }
  }

  // The art of a network, at the id of each BD_Feature. There is one complete
  // piece for each case of the connections. The prefix of a file is the name of
  // the feature, so this loop reads each feature that exists and names none of
  // them.
  for(BD_Feature feature = 0; feature < BD_Feature_COUNT; feature += 1) {
    for(U32 code = 1; code < 16; code += 1) {
      for(U32 variant = 0;; variant += 1) {
        String8 path = push_str8f(scratch.arena, "assets/tiles/%S_%u_%u.png",
                                  GM_FEATURE_NAMES[feature], code, variant);
        U32 id = map__load(map, scratch.arena, path);
        if(id == 0) { break; }
        tl_push_network(&map->tiling, feature, code, id);
      }
    }
  }

  // The art of a pawn, at the id of each GM_Sprite. The game layer says what a
  // thing looks like, and these files hold that appearance. This loop searches
  // for the variants, as it does for each other kind of tile art. A sprite with
  // no file keeps a count of 0, and the renderer then draws a flat shape. The
  // sprite 0 is the nil sprite, and it has no art.
  for(GM_Sprite sprite = 1; sprite < GM_Sprite_COUNT; sprite += 1) {
    for(U32 variant = 0; variant < MAP_PAWN_VARIANT_CAP; variant += 1) {
      String8 path = push_str8f(scratch.arena, "assets/tiles/site_%S_%u.png",
                                GM_SPRITE_NAMES[sprite], variant);
      U32 id = map__load(map, scratch.arena, path);
      if(id == 0) { break; }
      map->gm_sprites[sprite][variant] = id;
      map->gm_sprite_counts[sprite] = variant + 1;
    }
  }

  d_spritesheet_end();
  arena_release_scratch(scratch);
  return map;
}


// The order of the draw: the layers of the tiler first, then the shade of each
// tile, then the items of the surface, then the marks above everything. The
// layers of the tiler come first because of how the art is built: a boundary
// that goes into the tile next to it must not cover a piece of overlay art.
#define MAP_LAYER_SHADING   TL_Layer_COUNT
#define MAP_LAYER_SURFACE   (TL_Layer_COUNT + 1)
#define MAP_LAYER_HIGHLIGHT (TL_Layer_COUNT + 2)
#define MAP_LAYER_COUNT     (TL_Layer_COUNT + 3)

// The alpha of the color that shades a tile, which sets how much of the ground
// below that color a person sees.
#define MAP_SHADING_ALPHA 0.35f

// The rect in world space of the tile at (x, y). A position with a fraction is
// on the dual grid. The rect becomes smaller by `inset` units of the world at
// each side.
internal Rect map_tile_rect(F32 x, F32 y, F32 inset) {
  return (Rect){{x * MAP_TILE + inset, y * MAP_TILE + inset},
                {(x + 1) * MAP_TILE - inset, (y + 1) * MAP_TILE - inset}};
}

// The cell of the tile below a position on the screen. That position is in the
// points of the client area, as wnd_mouse_pos gives it.
//
// This function rounds toward the lower value, so a cell to the west of the
// board and a cell to the north of it come back with a value below 0. The
// caller must test the result against the size of the board.
internal V2I map_tile_from_screen(D_Camera camera, V2 screen) {
  V2 world = d_camera_from_screen(camera, screen);
  V2I tile = {(I32)(world.x / MAP_TILE), (I32)(world.y / MAP_TILE)};
  // A cast to I32 rounds toward 0. The subtraction of the result of the
  // comparison changes that into a round toward the lower value, which is what
  // a coordinate below 0 needs.
  tile.x -= (F32)tile.x * MAP_TILE > world.x;
  tile.y -= (F32)tile.y * MAP_TILE > world.y;
  return tile;
}

internal void map_draw(Map_View* map, GM_MapItems items, D_Camera camera) {
  // One shape that this module can draw now. A sprite of 0 says that the shape
  // is a color.
  typedef struct {
    Rect rect;
    U32 sprite; // an index into map->sprites. A value of 0 says that there is none.
    U32 mask;   // the mask of the sprite. A value of 0 says that there is none.
    V4 color;   // The tint of the sprite, where a value of 0 keeps its colors. Where sprite is 0, it is the color of the shape.
    F32 rounding;
    F32 outline; // A value above 0 draws an outline of that thickness, in units of the world, and does not fill the shape.
  } MapDrawCmd;

  V2 vp = wnd_size();
  d_rect((Rect){{0, 0}, {vp.x, vp.y}}, (V4){0.06f, 0.06f, 0.08f, 1});

  ArenaTemp scratch = arena_get_scratch(0, 0);

  //- Find the size of each layer, which is one slot for each piece and for each
  //  item of the surface. Those sizes are upper limits, because a piece that
  //  gives nothing leaves its slot empty. This pass also fills the cache of the
  //  cells, so the pass below reads that cache.
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
    if(item->color.w > 0) { cap[MAP_LAYER_SHADING] += 1; }
  }
  U64 base[MAP_LAYER_COUNT];
  U64 total = 0;
  for(U32 layer = 0; layer < MAP_LAYER_COUNT; layer += 1) {
    base[layer] = total;
    total += cap[layer];
  }
  MapDrawCmd* cmds = push_array_no_zero(scratch.arena, MapDrawCmd, total);
  U64 count[MAP_LAYER_COUNT] = {0};

  //- Read each item one time, and put each shape in a slot of its layer.
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
    if(!item->has_pawn) { // The cells of the ground, with the ring outside the board. The pass above counts the same cells.
      TL_Cell* cell = map__cell(map, item);
      for(U32 pi = 0; pi < cell->count; pi += 1) {
        TL_Piece* piece = &cell->pieces[pi];
        // There is no tint here. A piece of the ground is on the dual grid,
        // half a cell from the tile of this item, so a color of one tile
        // cannot go on such a piece.
        Rect r = map_tile_rect(item->pos.x + piece->offset.x, item->pos.y + piece->offset.y, 0);
        MapDrawCmd* cmd = &cmds[base[piece->layer] + count[piece->layer]];
        *cmd = (MapDrawCmd){.rect = r, .sprite = piece->id, .mask = piece->mask_id};
        count[piece->layer] += 1;
      }

      // The shade takes its own quad, on the grid of the map, where the tile
      // is.
      if(item->color.w > 0) {
        MapDrawCmd* cmd = &cmds[base[MAP_LAYER_SHADING] + count[MAP_LAYER_SHADING]];
        count[MAP_LAYER_SHADING] += 1;
        *cmd = (MapDrawCmd){
            .rect = map_tile_rect(item->pos.x, item->pos.y, 0),
            .color = col_with_alpha(item->color, MAP_SHADING_ALPHA),
        };
      }
    }

    if(item->has_pawn) {
      U32 variant_count = map->gm_sprite_counts[item->sprite];

      F32 inset = 0.0;
      F32 rounding = 0.0;
      U32 sprite = 0;

      if(variant_count > 0) {
        // The whole tile. The art holds its own shape, and the color of the
        // item goes on it as a tint, where white keeps the colors of the art.
        // A hash of the id of the thing chooses the variant, so that variant
        // stays equal for the life of the thing, and two things take two
        // variants.
        U32 variant = (U32)(rng_hash_u64(0, item->id) % variant_count);
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

  //- Draw the layers in their order, and the shapes of each layer in the order
  //  of the pass above.
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

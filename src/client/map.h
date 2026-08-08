#pragma once
#include "base/core.h"
#include "base/math.h"
#include "base/arena.h"
#include "game/game.h"
#include "gfx/draw.h"

////////////////////////////////
//~ fp: Map
//
// The client's view of the world -- the world itself is game/'s (worldgen
// knobs in data/world.tabula, terrain rows in data/terrain_types.tabula).
// One opaque object holds the sprite assets,
// the tiling registration built while loading them, and the tiler cell
// cache. Pure presentation: reads no input; the camera arrives as a value.

#define MAP_TILE 8.0f // world units per tile

typedef struct Map_View Map_View;

internal Map_View* map_init(Arena* arena); // loads assets; wg_terrain_table_load must have run
// (re)size the cell cache to a new world; cells go on `arena`
internal void map_world_changed(Map_View* map, Arena* arena, I32 board_w, I32 board_h);
internal V2I map_tile_from_screen(D_Camera camera, V2 screen);
internal void map_draw(Map_View* map, GM_MapItems items, D_Camera camera);

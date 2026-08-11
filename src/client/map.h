#pragma once
#include "base/core.h"
#include "base/math.h"
#include "base/arena.h"
#include "game/game.h"
#include "gfx/draw.h"

////////////////////////////////
//~ fp: Map
//
// The view of the world of the client. The world itself belongs to the game
// layer. data/world.tabula holds the parameters of the generator, and
// data/terrain_types.tabula holds the terrain rows.
//
// One object holds the sprites, the registration of those sprites in the
// tiler, and the cache of the cells of the tiler. A caller does not read the
// members of that object.
//
// This module draws and does nothing more. It reads no input, and the camera
// arrives as a value.

#define MAP_TILE 8.0f // the units of the world for each tile

typedef struct Map_View Map_View;

internal Map_View* map_init(Arena* arena); // It loads the assets. A caller must call wg_terrain_table_load first.
// Set the size of the cache of the cells to the size of a new world. The
// cells go on an arena that the view owns, and this call clears it.
internal void map_world_changed(Map_View* map, I32 board_w, I32 board_h);
internal V2I map_tile_from_screen(D_Camera camera, V2 screen);
internal void map_draw(Map_View* map, GM_MapItems items, D_Camera camera);

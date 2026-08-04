#pragma once

#include "base/core.h"
#include "base/math.h"
#include "base/arena.h"
#include "base/strings.h"
#include "tabula.h"
#include "game/board.h"

////////////////////////////////
//~ fp: Worldgen
//
// Procedural world generation: turns generation knobs -- loaded from a tabula
// file -- into a populated board. This is where board terrain ids get their
// meaning: they are WG_TerrainType values, described by the WG_TERRAIN_DATA
// table below.
//
// Generation is deterministic: same params, same seed, same world. All
// randomness is integer-hash noise keyed on params.seed -- there is no rng
// state anywhere.

////////////////////////////////
//~ fp: Terrain Types
//
// The closed set of terrain kinds, with a data table indexed by type. Nil is
// zero (ZII: a tile never painted reads as Nil -- impassable and rendered in
// the loud bad-id color).

typedef U16 WG_TerrainType; // width-compatible with BD_Terrain
enum {
  WG_TerrainType_Nil,
  WG_TerrainType_Water,
  WG_TerrainType_Plains,
  WG_TerrainType_Forest,
  WG_TerrainType_Mountain,
  WG_TerrainType_COUNT,
};

typedef struct {
  String8 name;
  V4 color;      // map render color
  F32 move_cost; // cost to enter, <= 0 impassable (BD_TravelRules semantics)
} WG_TerrainData;

global WG_TerrainData WG_TERRAIN_DATA[WG_TerrainType_COUNT] = {
  {str8_lit_comp("nil"),      {1.00f, 0.00f, 1.00f, 1}, 0},    // loud magenta
  {str8_lit_comp("water"),    {0.18f, 0.32f, 0.55f, 1}, 0},
  {str8_lit_comp("plains"),   {0.47f, 0.62f, 0.33f, 1}, 1.0f},
  {str8_lit_comp("forest"),   {0.23f, 0.42f, 0.24f, 1}, 2.0f},
  {str8_lit_comp("mountain"), {0.54f, 0.52f, 0.50f, 1}, 0},
};

////////////////////////////////
//~ fp: Generation Parameters
//
// Every knob the generator reads, filled from the file's `world` object with
// workable defaults for anything missing. Loading never fails hard: a missing
// or broken file reports to stderr (mirroring tabula itself) and yields pure
// defaults.

typedef struct {
  I32 width;
  I32 height;

  //- fp: noise fields; scale is tiles per noise-lattice cell
  F32 elevation_scale;
  I32 elevation_octaves;
  F32 moisture_scale;
  I32 moisture_octaves;

  //- fp: classification thresholds over the [0,1] noise fields
  F32 sea_level;       // elevation below -> water
  F32 mountain_level;  // elevation above -> mountain
  F32 forest_moisture; // moisture above (on remaining land) -> forest

  // how strongly elevation sinks toward the map edge (0 = none); pulls the
  // coastline inward until the land becomes one continent
  F32 continent_falloff;

  I32 river_count;

  //- fp: travel rule scalars forwarded onto the board
  F32 road_cost;
  F32 river_cross_cost;
} WG_Params;

internal WG_Params wg_params_load(String8 path);

////////////////////////////////
//~ fp: Generation
//
// Allocates a board on `arena`, paints terrain from the noise fields, carves
// rivers downhill from high ground, and installs travel rules from
// WG_TERRAIN_DATA. Overwrite params.seed before calling to get a different
// world from the same knobs.

internal BD_Board* wg_generate(Arena* arena, WG_Params* params, U64 seed);

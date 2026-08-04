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
// randomness is integer-hash noise keyed on the seed passed to wg_generate --
// there is no rng state anywhere.

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
  WG_TerrainType_Desert,
  WG_TerrainType_Swamp,
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
  {str8_lit_comp("desert"),   {0.78f, 0.70f, 0.48f, 1}, 1.5f},
  {str8_lit_comp("swamp"),    {0.30f, 0.42f, 0.36f, 1}, 3.0f},
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

  //- fp: classification thresholds. Elevation splits water / land / mountain;
  //  the land in between is a biome matrix of moisture (rain arriving, its
  //  own noise field) against drainage (water leaving -- not a field of its
  //  own, but read off the elevation landscape: slope sheds water, height
  //  stands above the water table)
  F32 sea_level;       // elevation below -> water
  F32 mountain_level;  // elevation above -> mountain
  F32 desert_moisture; // moisture below -> desert
  F32 forest_moisture; // moisture above -> forest
  F32 swamp_moisture;  // moisture above, and...
  F32 swamp_drainage;  // ...drainage below -> swamp (beats forest)
  F32 river_moisture;  // extra moisture on and beside river tiles

  // how strongly elevation sinks toward the map edge (0 = none); pulls the
  // coastline inward until the land becomes one continent
  F32 continent_falloff;

  I32 river_count;
  I32 river_min_length; // steps; rivers that trace shorter than this are culled
  F32 river_meander;    // jitter on candidate elevations during descent, in
                        // elevation units; 0 = pure steepest descent
  F32 pond_epsilon;     // when a river dies in a basin, the connected bowl of
                        // land within this height of the basin floods with it
  I32 pond_max_tiles;   // hard cap on one pond's flood, whatever the epsilon

  //- fp: travel rule scalars forwarded onto the board
  F32 road_cost;
  F32 river_cross_cost;
} WG_Params;

internal WG_Params wg_params_load(String8 path);

////////////////////////////////
//~ fp: Generation
//
// Allocates a board on `arena`, carves rivers downhill from high ground,
// classifies terrain from the fields (rivers first, so their valleys read as
// wetter land), and installs travel rules from WG_TERRAIN_DATA. The params
// describe a family of worlds; `seed` picks the member.

internal BD_Board* wg_generate(Arena* arena, WG_Params* params, U64 seed);

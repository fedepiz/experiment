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
// Procedural world generation: turns generation knobs and terrain rows --
// both loaded from a tabula file -- into a populated board. This is where
// board terrain ids get their meaning: they index the terrain rows below.
//
// Generation is deterministic: same params, same seed, same world. All
// randomness is integer-hash noise keyed on the seed passed to wg_generate --
// there is no rng state anywhere.

////////////////////////////////
//~ fp: Terrain Definitions
//
// Terrain is data: each `terrain_type = { ... }` entry in the world file
// becomes one row here, in file order, and the row index is the terrain id
// everywhere (board tiles, tiling classes, art prefixes). One fat row per
// terrain carries what every consumer reads: identity and art naming, map
// fallback color, boundary rank, overlay density, travel cost, and the
// classification bands.
//
// Classification is ordered FIRST MATCH: a tile takes the first row whose
// declared bands all contain the tile's field values. Bands are closed
// intervals; a zero band reads as "don't care" (ZII), so an all-zero row is
// a catch-all -- the file's last row should be one. Row 0 is the reserved
// nil terrain (ZII: an unpainted tile reads as nil -- impassable, loud
// magenta); the classifier starts at row 1, and a tile no row claims stays
// nil (wg_params_load reports any such gap in the band data).

#define WG_TERRAIN_CAP 16

typedef struct {
  F32 min;
  F32 max;
} WG_Band; // zero band = don't care

typedef struct {
  String8 name;        // asset prefix and display name; lives on the load arena
  V4 color;            // flat map color when a terrain has no ground art
  U8 rank;             // boundary covering order; higher spills over lower
  U32 overlay_density; // percent of interior cells carrying an overlay
  F32 move_cost;       // cost to enter, <= 0 impassable (BD_TravelRules semantics)

  //- classification bands
  WG_Band elevation;
  WG_Band moisture;
  WG_Band drainage;
  B32 needs_coast;     // only matches with the sea one step away
} WG_TerrainDef;


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

  //- fp: noise fields; scale is tiles per noise-lattice cell, persistence
  //  is the per-octave amplitude falloff (higher = rougher, more variation
  //  inside the large forms)
  F32 elevation_scale;
  I32 elevation_octaves;
  F32 elevation_persistence;
  F32 moisture_scale;
  I32 moisture_octaves;
  F32 moisture_persistence;

  //- fp: field semantics. Classification itself lives in the terrain rows;
  //  these anchor the fields the rows read: sea_level says where water sits
  //  (rivers trace to it, coasts test against it), drainage_ceiling is the
  //  elevation where the height-above-the-water-table drainage term
  //  saturates, river_moisture wets river banks before matching
  F32 sea_level;
  F32 drainage_ceiling;
  F32 river_moisture;

  //- fp: terrain rows, in file order (see WG_TerrainDef); [0] is baked nil
  WG_TerrainDef terrains[WG_TERRAIN_CAP];
  U32 terrain_count;

  //- fp: continental rim: the border is always deep water. Elevation
  //  interpolates from the untouched heightmap to 0 across a band inside
  //  the map edge: continent_edge is where the blend begins (distance from
  //  the border, in half-map units), continent_smoothness how much of that
  //  distance it takes to reach full water
  F32 continent_edge;
  F32 continent_smoothness;

  I32 river_count;      // rivers actually carved (the quota)...
  I32 river_max_tries;  // ...and the source attempts spent chasing it
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

// terrain names are pushed onto `arena`; everything else is by value
internal WG_Params wg_params_load(Arena* arena, String8 path);

////////////////////////////////
//~ fp: Generation
//
// Allocates a board on `arena`, carves rivers downhill from high ground,
// classifies terrain from the fields (rivers first, so their valleys read as
// wetter land), and installs travel rules from the terrain rows. The params
// describe a family of worlds; `seed` picks the member.

internal BD_Board* wg_generate(Arena* arena, WG_Params* params, U64 seed);

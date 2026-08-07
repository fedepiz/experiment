#pragma once

#include "base/core.h"
#include "base/math.h"
#include "base/arena.h"
#include "base/strings.h"
#include "tabula.h"
#include "game/thing_db.h"

////////////////////////////////
//~ fp: Worldgen
//
// Procedural world generation: turns generation knobs and terrain rows --
// both loaded from a tabula file -- into a populated world, written as
// fields into the thing database (TH_IField_Terrain and the feature masks).
// This is where terrain ids get their meaning: they index the terrain type
// table below.
//
// Generation is deterministic: same params, same seed, same world. All
// randomness is integer-hash noise keyed on the seed passed to wg_generate --
// there is no rng state anywhere.

////////////////////////////////
//~ fp: Terrain Types
//
// What a terrain *is*, apart from where generation puts it: the cross-module
// terrain registry -- the row index is the terrain id everywhere
// (TH_IField_Terrain values, tiling classes, board travel costs, art
// prefixes, map colors). Filled whole by wg_terrain_table_load from the
// world file's terrain_type rows, in file order, so repeated loads stay
// idempotent; call it before anything that reads the table. Row 0 is the
// reserved nil terrain (ZII: an unpainted cell reads as nil -- impassable,
// loud magenta). Names are copied into the rows, so the table outlives any
// load arena.

#define WG_TERRAIN_CAP 24
#define WG_TERRAIN_NAME_CAP 31

typedef struct {
  U8 name_len;                       // asset prefix and display name,
  U8 name_chars[WG_TERRAIN_NAME_CAP]; // read via wg_terrain_name
  V4 color;            // flat map color when a terrain has no ground art
  U8 rank;             // boundary covering order; higher spills over lower
  U32 overlay_density; // percent of interior cells carrying an overlay
  F32 move_cost;       // cost to enter, <= 0 impassable (BD_TravelRules semantics)
} WG_TerrainType;

global WG_TerrainType WG_TERRAIN_TYPES[WG_TERRAIN_CAP];
global U32 WG_TERRAIN_TYPE_COUNT;

internal void wg_terrain_table_load(String8 path);
internal String8 wg_terrain_name(U32 type); // empty past the count
internal U32 wg_terrain_by_name(String8 name); // 0 (nil) when unknown

////////////////////////////////
//~ fp: Terrain Classification
//
// The generation-only half of a terrain row: which field values claim it.
// Classification is ordered FIRST MATCH: a tile takes the first row whose
// declared bands all contain the tile's field values. Bands are closed
// intervals; a zero band reads as "don't care" (ZII), so an all-zero row is
// a catch-all -- the file's last row should be one. The classifier starts at
// row 1, and a tile no row claims stays nil (wg_params_load reports any such
// gap in the band data).

typedef struct {
  F32 min;
  F32 max;
} WG_Band; // zero band = don't care

typedef struct {
  WG_Band elevation;
  WG_Band moisture;
  WG_Band drainage;
  WG_Band temperature;
  B32 needs_coast; // only matches with the sea one step away
} WG_TerrainDef;


////////////////////////////////
//~ fp: Generation Parameters
//
// Every knob the generator reads, filled from the file's `world` object. All
// parse fallbacks are ZERO on purpose: the file is the single source of
// truth, and a missing or misspelled key must produce an obviously broken
// world, never a quietly substituted default. Knobs whose zero would poison
// the math (divisors, octave counts) are Asserted after the parse; loading
// itself still never fails hard -- a broken file reports to stderr
// (mirroring tabula itself).

typedef struct {
  I32 width;
  I32 height;

  //- fp: noise fields; scale is tiles per noise-lattice cell, persistence
  //  is the per-octave amplitude falloff (higher = rougher, more variation
  //  inside the large forms)
  F32 elevation_scale;
  I32 elevation_octaves;
  F32 elevation_persistence;
  F32 elevation_amplitude; // base noise ceiling, 0..1: noise spans
                           // [0, amplitude] the way uplift spans
                           // [0, uplift_height]; the two stack
  F32 moisture_scale;
  I32 moisture_octaves;
  F32 moisture_persistence;

  //- fp: temperature: a north/equator/south gradient (piecewise-linear by
  //  latitude; the equator point is the map's middle row), plus noise
  //  wobble, minus a lapse cooling for land standing above the sea
  F32 temperature_north;
  F32 temperature_equator;
  F32 temperature_south;
  F32 temperature_variation;
  F32 temperature_scale;
  I32 temperature_octaves;
  F32 temperature_persistence;
  F32 temperature_lapse;          // drop at the world's elevation ceiling...
  F32 temperature_lapse_exponent; // ...curved: >1 spares mid ground and
                                  // freezes ridgelines sharply

  //- fp: field semantics. Classification itself lives in the terrain rows;
  //  these anchor the fields the rows read: sea_level says where water sits
  //  (rivers trace to it, coasts test against it), drainage_ceiling is the
  //  elevation where the height-above-the-water-table drainage term
  //  saturates, river_moisture wets river banks before matching
  F32 sea_level;
  F32 drainage_ceiling;
  F32 drainage_full_slope; // slope, in elevation per tile, that counts as fully drained
  F32 river_moisture;

  // terrain regions smaller than this dissolve into their most common
  // neighbor after classification; 0 disables. Band classification flips
  // single tiles wherever a field grazes a threshold, and those specks
  // read as noise, not terrain.
  I32 min_region_size;

  //- fp: classification bands, one row per terrain type, in file order
  //  (see WG_TerrainDef); [0] is the baked nil. Mirrors WG_TERRAIN_TYPES.
  WG_TerrainDef terrains[WG_TERRAIN_CAP];
  U32 terrain_count;

  //- fp: tectonic plates: the map splits into fuzzy voronoi cells around
  //  blue-noise seed points (one per plate_spacing-sized grid cell, so plate
  //  size is map-scale independent). Each plate drifts with a hashed
  //  velocity; borders whose relative motion presses inward seed ridges.
  //  Uplift falls off with distance to the nearest ridge -- the distance
  //  warped by noise so ranges pinch and wander -- and is shaped by ridged
  //  noise so crests break into peaks and saddles instead of smooth walls.
  F32 plate_spacing;       // mean tiles between plate seeds
  F32 plate_fuzz;          // border warp amplitude, tiles; 0 = exact voronoi
  F32 plate_fuzz_scale;    // tiles per warp-noise cell
  F32 uplift_height;       // elevation added at a full-force ridge
  F32 uplift_width;        // tiles from ridge spine to the foot of the falloff
  F32 uplift_noise;        // distance warp strength, 0..1
  F32 uplift_noise_scale;  // tiles per distance-warp-noise cell
  F32 uplift_ridged;       // ridged-noise shaping blend, 0..1
  F32 uplift_ridged_scale; // tiles per ridged-noise cell

  //- fp: divergent borders -- plates pulling apart -- express by crust:
  //  on land they sink a rift valley (long linear lake chains where the
  //  floor drops below sea level), in drowned ocean they raise a mid-ocean
  //  ridge that can breach the surface as volcanic island arcs. One field,
  //  blended by the rim.
  F32 rift_depth;  // elevation removed at a full-force rift on land
  F32 rift_width;  // tiles from rift axis to the foot of the falloff
  F32 arc_height;  // elevation added at a full-force ocean ridge

  //- fp: continental rim, plate-shaped: every plate owning a border tile
  //  drowns to zero elevation, and land climbs back to the full heightmap
  //  over continent_blend tiles inland of the drowned region, so coastlines
  //  follow the fuzzy plate borders instead of the map rectangle.
  //  continent_height is the floor interior plates stand on -- the
  //  continental shelf above the abyss -- so land-vs-sea comes from plate
  //  structure while noise and uplift carve relief on top.
  F32 continent_blend;
  F32 continent_height;

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

internal WG_Params wg_params_load(String8 path);

////////////////////////////////
//~ fp: Generation
//
// Writes a world into the thing database: sets the world size (asserted
// against TH_WORLD_MAX_DIM), carves rivers downhill from high ground, and
// classifies terrain from the fields (rivers first, so their valleys read as
// wetter land). Everything lands in TH_ fields -- TH_IField_Terrain and the
// feature masks; previous field contents are overwritten. The params
// describe a family of worlds; `seed` picks the member.

internal void wg_generate(TH_Db* db, WG_Params* params, U64 seed);

// Feature masks are kept mirrored: connecting p toward d also sets the
// opposite bit on p's neighbor. At the world edge the neighbor half is
// simply dropped -- a river may flow off the world.
internal void wg_field_connect(TH_Db* db, V2I p, Dir4 dir, TH_IField mask_field);

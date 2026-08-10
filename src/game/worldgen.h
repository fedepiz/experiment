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
// World generation makes a world from a set of parameters and a set of terrain
// rows. It reads both from a tabula file. It writes the world into the fields
// of the thing database: TH_IField_Terrain and the feature masks.
//
// A terrain id gets its meaning here. The id is the index of a row of the
// terrain table below.
//
// Generation is deterministic. The same parameters and the same seed give the
// same world. Each random value is a hash of integers, and the seed of
// wg_generate is one of those integers. There is no state for a random number
// generator.

////////////////////////////////
//~ fp: Terrain Types
//
// The terrain table says what a terrain is, apart from the places where
// generation puts it. Each module reads the table. The index of a row is the
// terrain id everywhere: in a TH_IField_Terrain value, in a tiling class, in a
// travel cost of the board, in an art prefix, and in a map color. Each row
// also holds the bands that claim it.
//
// wg_terrain_table_load fills the whole table from the rows of the terrain
// file, in the order of the file. A second load therefore gives the same
// table. Call it before any code that reads the table, and before wg_generate,
// which classifies each tile against the table.
//
// Row 0 is the nil terrain (ZII). A cell that generation does not paint reads
// as nil, which is impassable and bright magenta.
//
// The load copies each name into its row, so the table stays valid after the
// arena of the load is gone.

#define WG_TERRAIN_CAP      24
#define WG_TERRAIN_NAME_CAP 31

typedef U64 WG_TerrainFlags;

enum {
  WG_TerrainFlag_Fertile = (1 << 0),
};

typedef struct {
  F32 min;
  F32 max;
} WG_Band; // a band of 0 to 0 accepts each value

typedef struct {
  U8 name_len;                        // the art prefix, and the name to show.
  U8 name_chars[WG_TERRAIN_NAME_CAP]; // Read it with wg_terrain_name.
  V4 color;                           // the map color, for a terrain with no ground art
  U8 rank;                            // the order at a boundary. A higher rank covers a lower one.
  U32 overlay_density;                // the percent of the inner cells that carry an overlay
  F32 move_cost;                      // the cost to enter. A cost of 0 or less
                                      // is impassable, as in BD_TravelRules.
  WG_TerrainFlags flags;              // the traits of the terrain type

  //- fp: the field values that claim this row. See Terrain Classification below.
  WG_Band elevation;
  WG_Band moisture;
  WG_Band drainage;
  WG_Band temperature;
  B32 needs_coast; // the row claims a tile only when the sea is one step away
} WG_TerrainType;

global WG_TerrainType WG_TERRAIN_TYPES[WG_TERRAIN_CAP];
global U32 WG_TERRAIN_TYPE_COUNT;

internal void wg_terrain_table_load(String8 path);
internal String8 wg_terrain_name(U32 type);    // empty for an index past the count
internal U32 wg_terrain_by_name(String8 name); // 0, the nil terrain, for an unknown name
internal const WG_TerrainType* wg_terrain_type_get(U32 idx);

// The flag of this name. A name that the table does not hold gives 0, which
// the caller must report: a wrong spelling must not read as "no flag".
internal WG_TerrainFlags wg_terrain_flag_by_name(String8 name);

////////////////////////////////
//~ fp: Terrain Classification
//
// The WG_Band members above say which field values claim a terrain row.
//
// Classification takes the first row that matches. A tile takes the first row
// whose bands all contain the field values of that tile. A band is a closed
// interval. A band of 0 to 0 accepts each value (ZII), so a row with no band
// accepts each tile. Make the last row of the file such a row.
//
// The classifier starts at row 1. A tile that no row claims stays nil.
// wg_terrain_table_load reports a gap of this kind in the band data.


////////////////////////////////
//~ fp: Generation Parameters
//
// Each parameter that the generator reads. The parse fills them from the
// `world` object of the file.
//
// Each parse fallback is 0, and that is deliberate. The file is the only
// source of these values. A key that is absent or that has a wrong spelling
// must give an obviously broken world, and never a default that a reader
// cannot see.
//
// An assert after the parse tests each parameter whose value of 0 would break
// the arithmetic, such as a divisor and a count of octaves. The load itself
// does not stop the program. A broken file writes a report to stderr, as
// tabula does.

typedef struct {
  I32 width;
  I32 height;

  //- fp: the noise fields. A scale is the tiles for each cell of the noise
  //  grid. A persistence is the decrease of the amplitude at each octave. A
  //  larger persistence gives a rougher field, with more variation inside the
  //  large shapes.
  F32 elevation_scale;
  I32 elevation_octaves;
  F32 elevation_persistence;
  F32 elevation_amplitude; // the ceiling of the base noise, from 0 to 1. The
                           // noise covers [0, amplitude], as the uplift covers
                           // [0, uplift_height]. The generator adds the two.
  F32 moisture_scale;
  I32 moisture_octaves;
  F32 moisture_persistence;

  //- fp: the temperature. It is a gradient from the north to the equator to
  //  the south, which is linear between those three points. The equator is the
  //  middle row of the map. The generator then adds a noise, and subtracts a
  //  fall in temperature for the land that stands above the sea.
  F32 temperature_north;
  F32 temperature_equator;
  F32 temperature_south;
  F32 temperature_variation;
  F32 temperature_scale;
  I32 temperature_octaves;
  F32 temperature_persistence;
  F32 temperature_lapse;          // the fall at the highest elevation of the world
  F32 temperature_lapse_exponent; // the curve of that fall. A value above 1
                                  // keeps the middle ground warm and makes the
                                  // high ridges cold.

  //- fp: the meaning of the fields. The terrain rows hold the classification
  //  itself. These values fix the fields that the rows read. sea_level is the
  //  height of the water: a river runs down to it, and a coast test uses it.
  //  drainage_ceiling is the elevation where the term for the height above the
  //  water table reaches its largest value. river_moisture wets the banks of a
  //  river before the classification.
  F32 sea_level;
  F32 drainage_ceiling;
  F32 drainage_full_slope; // the slope, in elevation for each tile, that counts as fully drained
  F32 river_moisture;

  // A region of terrain that is smaller than this becomes the terrain of its
  // most common neighbour, after the classification. A value of 0 stops this
  // step. The band classification changes single tiles wherever a field is
  // near a limit, and a person reads those single tiles as noise and not as
  // terrain.
  I32 min_region_size;

  //- fp: the tectonic plates. The map divides into voronoi cells with rough
  //  borders. Each cell grows around a seed point, and there is one seed point
  //  for each grid cell of the size plate_spacing. The size of a plate is
  //  therefore the same at any size of map.
  //
  //  Each plate moves with a velocity that a hash gives it. A border where two
  //  plates press together makes a ridge. The uplift decreases with the
  //  distance to the nearest ridge, and a noise moves that distance, so a
  //  range of mountains becomes narrow in places and turns. A ridged noise
  //  then shapes the uplift, so a crest breaks into peaks and passes and does
  //  not stay a smooth wall.
  F32 plate_spacing;       // the mean tiles between two plate seeds
  F32 plate_fuzz;          // the size of the border warp, in tiles. 0 gives an exact voronoi cell.
  F32 plate_fuzz_scale;    // the tiles for each cell of the warp noise
  F32 uplift_height;       // the elevation that a ridge at full force adds
  F32 uplift_width;        // the tiles from the line of a ridge to the foot of the fall
  F32 uplift_noise;        // the strength of the distance warp, from 0 to 1
  F32 uplift_noise_scale;  // the tiles for each cell of the distance warp noise
  F32 uplift_ridged;       // the part of the ridged noise in the shape, from 0 to 1
  F32 uplift_ridged_scale; // the tiles for each cell of the ridged noise

  //- fp: the borders where two plates move apart. The result depends on the
  //  crust. On land such a border sinks a rift valley, and the floor of that
  //  valley goes below the sea level in places and makes a long line of lakes.
  //  Under the sea such a border raises a ridge, which can reach the surface
  //  and make a line of volcanic islands. One field holds both, and the rim
  //  below mixes them.
  F32 rift_depth; // the elevation that a rift at full force removes on land
  F32 rift_width; // the tiles from the axis of a rift to the foot of the fall
  F32 arc_height; // the elevation that an ocean ridge at full force adds

  //- fp: the rim of a continent, which follows the plates. Each plate that
  //  owns a tile at the border of the map falls to an elevation of 0. The land
  //  then rises to the full height map over continent_blend tiles inland of
  //  that area. A coast therefore follows the rough border of a plate, and not
  //  the rectangle of the map.
  //
  //  continent_height is the floor that an inner plate stands on, which is the
  //  shelf above the deep sea. The structure of the plates therefore decides
  //  land against sea, and the noise and the uplift shape the relief above it.
  F32 continent_blend;
  F32 continent_height;

  I32 river_count;      // the rivers to carve
  I32 river_max_tries;  // the source points to try, to reach that count
  I32 river_min_length; // the steps of the shortest river. The generator removes a shorter one.
  F32 river_meander;    // the change that the generator adds to each candidate
                        // elevation as a river goes down, in elevation units.
                        // A value of 0 gives the steepest path down.
  F32 pond_epsilon;     // A river can stop in a basin. The connected land
                        // within this height of the floor of that basin then
                        // fills with water.
  I32 pond_max_tiles;   // the largest number of tiles of one pond, at any epsilon

  //- fp: the travel rule values that the game passes to the board
  F32 road_cost;
  F32 river_cross_cost;
} WG_Params;

internal WG_Params wg_params_load(String8 path);

////////////////////////////////
//~ fp: Generation
//
// wg_generate writes a world into the thing database. It sets the size of the
// world, and an assert tests that size against TH_WORLD_MAX_DIM. It then
// carves each river down from the high ground, and classifies the terrain from
// the fields. It carves the rivers first, so the valley of a river reads as
// wetter land.
//
// Each result goes into a TH_ field: TH_IField_Terrain and the feature masks.
// The generator writes over the values that those fields held.
//
// The parameters describe a family of worlds. `seed` chooses one member of
// that family.

internal void wg_generate(TH_Db* db, WG_Params* params, U64 seed);

// The two halves of a feature mask always agree. A connection from p toward d
// also sets the opposite bit at the neighbour of p. At the edge of the world
// the function writes the half at p only, so a river can flow off the world.
internal void wg_field_connect(TH_Db* db, V2I p, Dir4 dir, TH_IField mask_field);

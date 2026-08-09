#include "base/core.h"
#include "base/math.h"
#include "base/rng.h"
#include "base/arena.h"
#include "base/strings.h"
#include "base/print.h"
#include "base/tctx.h"
#include "tabula.h"
#include "game/worldgen.h"

#include <math.h>

////////////////////////////////
//~ fp: Parameters

internal WG_Band wg__band_from_key(TB_Value* object, String8 key) {
  WG_Band band = {0};
  TB_Value* list = tb_get(object, key);
  if(list->kind == TB_ValueKind_List && list->first != 0) {
    band.min = tb_num_from_value(list->first, 0);
    band.max = tb_num_from_value(list->first->next, 0);
  }
  return band;
}

internal B32 wg__band_contains(WG_Band band, F32 v) {
  if(band.min == 0 && band.max == 0) { return 1; } // zero band = don't care
  return band.min <= v && v <= band.max;
}

// The first row after the nil row whose bands all contain the field values.
// The result is 0 when no row claims the tile.
internal U32 wg__classify(F32 e, F32 moisture, F32 drainage,
                          F32 temperature, B32 coast) {
  for(U32 i = 1; i < WG_TERRAIN_TYPE_COUNT; i += 1) {
    WG_TerrainType* def = &WG_TERRAIN_TYPES[i];
    if(!wg__band_contains(def->elevation, e)) { continue; }
    if(!wg__band_contains(def->moisture, moisture)) { continue; }
    if(!wg__band_contains(def->drainage, drainage)) { continue; }
    if(!wg__band_contains(def->temperature, temperature)) { continue; }
    if(def->needs_coast && !coast) { continue; }
    return i;
  }
  return 0;
}

// A set of bands is a box with sides that follow the axes. A gap in the
// coverage therefore contains the middle point of one cell of the grid that
// all the band limits make. A test of those middle points is an exact test.
// A gap writes a report to stderr, and each tile in it becomes the nil
// terrain, which is bright magenta.
internal void wg__report_band_gaps(String8 path) {
  F32 cuts[4][2 * WG_TERRAIN_CAP + 2];
  U32 cut_counts[4];
  for(U32 dim = 0; dim < 4; dim += 1) {
    cuts[dim][0] = 0;
    cuts[dim][1] = 1;
    cut_counts[dim] = 2;
  }
  for(U32 i = 1; i < WG_TERRAIN_TYPE_COUNT; i += 1) {
    WG_TerrainType* def = &WG_TERRAIN_TYPES[i];
    WG_Band bands[4] = {def->elevation, def->moisture, def->drainage, def->temperature};
    for(U32 dim = 0; dim < 4; dim += 1) {
      if(bands[dim].min == 0 && bands[dim].max == 0) { continue; }
      cuts[dim][cut_counts[dim]] = bands[dim].min;
      cuts[dim][cut_counts[dim] + 1] = bands[dim].max;
      cut_counts[dim] += 2;
    }
  }
  U32 gaps = 0;
  for(U32 ei = 0; ei + 1 < cut_counts[0] && gaps < 4; ei += 1) {
    for(U32 mi = 0; mi + 1 < cut_counts[1] && gaps < 4; mi += 1) {
      for(U32 di = 0; di + 1 < cut_counts[2] && gaps < 4; di += 1) {
        for(U32 ti = 0; ti + 1 < cut_counts[3] && gaps < 4; ti += 1) {
          F32 e = 0.5f * (cuts[0][ei] + cuts[0][ei + 1]);
          F32 m = 0.5f * (cuts[1][mi] + cuts[1][mi + 1]);
          F32 d = 0.5f * (cuts[2][di] + cuts[2][di + 1]);
          F32 t = 0.5f * (cuts[3][ti] + cuts[3][ti + 1]);
          if(wg__classify(e, m, d, t, 0) == 0) {
            eprintf_str8("%S: no terrain matches elevation %.2f moisture %.2f drainage %.2f temperature %.2f\n",
                         path, e, m, d, t);
            gaps += 1;
          }
        }
      }
    }
  }
}

internal String8 wg_terrain_name(U32 type) {
  if(type >= WG_TERRAIN_TYPE_COUNT) { return str8_lit(""); }
  return str8(WG_TERRAIN_TYPES[type].name_chars, WG_TERRAIN_TYPES[type].name_len);
}

internal const WG_TerrainType* wg_terrain_type_get(U32 idx) {
  Assert(idx < WG_TERRAIN_TYPE_COUNT);
  return &WG_TERRAIN_TYPES[idx];
}

internal U32 wg_terrain_by_name(String8 name) {
  for(U32 i = 0; i < WG_TERRAIN_TYPE_COUNT; i += 1) {
    if(str8_match(wg_terrain_name(i), name, 0)) { return i; }
  }
  return 0;
}

internal void wg__terrain_name_set(WG_TerrainType* type, String8 name) {
  type->name_len = (U8)ClampTop(name.size, WG_TERRAIN_NAME_CAP);
  MemoryCopy(type->name_chars, name.str, type->name_len);
}

typedef struct {
  const char* name;
  WG_TerrainFlags flags;
} WG_TerrainFlagsDef;

const WG_TerrainFlagsDef WG_TERRAIN_FLAG_DEFS[] = {
    {"fertile", WG_TerrainFlag_Fertile}};

internal void wg_terrain_table_load(String8 path) {
  ArenaTemp scratch = arena_get_scratch(0, 0);
  TB_Value* root = tb_parse_file_and_report(scratch.arena, path);

  //- fp: the terrain rows, in the order of the file. Row 0 is the nil row,
  //  which this code writes. The load fills the whole table, so a second load
  //  gives the same table.
  MemoryZeroArray(WG_TERRAIN_TYPES);
  {
    WG_TerrainType* nil_type = &WG_TERRAIN_TYPES[0];
    wg__terrain_name_set(nil_type, str8_lit("nil"));
    nil_type->color = (V4){1, 0, 1, 1}; // loud magenta
    WG_TERRAIN_TYPE_COUNT = 1;
  }
  for(TB_Node* node = root->first_member; node != 0; node = node->next) {
    if(!str8_match(node->key, str8_lit("terrain_type"), 0)) { continue; }
    if(WG_TERRAIN_TYPE_COUNT >= WG_TERRAIN_CAP) {
      eprintf_str8("%S: more than %d terrain_type entries; extras ignored\n",
                   path, WG_TERRAIN_CAP - 1);
      break;
    }
    TB_Value* src = &node->value;
    WG_TerrainType* type = &WG_TERRAIN_TYPES[WG_TERRAIN_TYPE_COUNT];
    WG_TERRAIN_TYPE_COUNT += 1;

    String8 name = tb_get_str8(src, str8_lit("name"), str8_lit("unnamed"));
    wg__terrain_name_set(type, name);
    type->color = tb_get_v4(src, str8_lit("color"), (V4){1, 0, 1, 1}); // magenta = the loud fallback
    type->rank = (U8)tb_get_num(src, str8_lit("rank"), 0);
    type->overlay_density = (U32)tb_get_num(src, str8_lit("overlay_density"), 0);
    type->move_cost = tb_get_num(src, str8_lit("move_cost"), 0); // missing = impassable, visibly

    type->elevation = wg__band_from_key(src, str8_lit("elevation"));
    type->moisture = wg__band_from_key(src, str8_lit("moisture"));
    type->drainage = wg__band_from_key(src, str8_lit("drainage"));
    type->temperature = wg__band_from_key(src, str8_lit("temperature"));
    type->needs_coast = tb_get_num(src, str8_lit("needs_coast"), 0) != 0;

    //- fp: the flags, which are a list of names from WG_TERRAIN_FLAG_DEFS. A
    //  row with no such list has no flag. A name that the table does not hold
    //  writes a report and adds nothing, so a wrong spelling cannot read as
    //  "this terrain does not have the trait".
    type->flags = 0;
    TB_Value* flag_list = tb_get(src, str8_lit("flags"));
    if(!tb_value_is_nil(flag_list) && flag_list->kind != TB_ValueKind_List) {
      eprintf_str8("%S: terrain '%S': flags must be a list\n", path, name);
    }
    for(TB_Value* elem = flag_list->first; elem != 0; elem = elem->next) {
      String8 flag_name = tb_str8_from_value(elem, str8_lit(""));
      B32 known = 0;
      for(U32 i = 0; i < ArrayCount(WG_TERRAIN_FLAG_DEFS); i += 1) {
        const WG_TerrainFlagsDef* def = &WG_TERRAIN_FLAG_DEFS[i];
        if(str8_match(str8_cstring(def->name), flag_name, 0)) {
          type->flags |= def->flags;
          known = 1;
          break;
        }
      }
      if(!known) {
        eprintf_str8("%S: terrain '%S': unknown flag '%S'\n", path, name, flag_name);
      }
    }
  }
  wg__report_band_gaps(path);
  arena_release_scratch(scratch);
}

internal WG_Params wg_params_load(String8 path) {
  ArenaTemp scratch = arena_get_scratch(0, 0);
  TB_Value* root = tb_parse_file_and_report(scratch.arena, path);
  TB_Value* world = tb_get(root, str8_lit("world"));

  // Each fallback is 0, and that is deliberate. A key that is absent or that
  // has a wrong spelling must give an obviously broken world, and not a
  // default that a reader cannot see. The file is the only source of these
  // values.
  WG_Params params = {0};
  params.width = (I32)tb_get_num(world, str8_lit("width"), 0);
  params.height = (I32)tb_get_num(world, str8_lit("height"), 0);

  params.elevation_scale = tb_get_num(world, str8_lit("elevation_scale"), 0);
  params.elevation_octaves = (I32)tb_get_num(world, str8_lit("elevation_octaves"), 0);
  params.elevation_persistence = tb_get_num(world, str8_lit("elevation_persistence"), 0);
  params.elevation_amplitude = tb_get_num(world, str8_lit("elevation_amplitude"), 0);
  params.moisture_scale = tb_get_num(world, str8_lit("moisture_scale"), 0);
  params.moisture_octaves = (I32)tb_get_num(world, str8_lit("moisture_octaves"), 0);
  params.moisture_persistence = tb_get_num(world, str8_lit("moisture_persistence"), 0);

  params.temperature_north = tb_get_num(world, str8_lit("temperature_north"), 0);
  params.temperature_equator = tb_get_num(world, str8_lit("temperature_equator"), 0);
  params.temperature_south = tb_get_num(world, str8_lit("temperature_south"), 0);
  params.temperature_variation = tb_get_num(world, str8_lit("temperature_variation"), 0);
  params.temperature_scale = tb_get_num(world, str8_lit("temperature_scale"), 0);
  params.temperature_octaves = (I32)tb_get_num(world, str8_lit("temperature_octaves"), 0);
  params.temperature_persistence = tb_get_num(world, str8_lit("temperature_persistence"), 0);
  params.temperature_lapse = tb_get_num(world, str8_lit("temperature_lapse"), 0);
  params.temperature_lapse_exponent = tb_get_num(world, str8_lit("temperature_lapse_exponent"), 0);

  params.sea_level = tb_get_num(world, str8_lit("sea_level"), 0);
  params.drainage_ceiling = tb_get_num(world, str8_lit("drainage_ceiling"), 0);
  params.drainage_full_slope = tb_get_num(world, str8_lit("drainage_full_slope"), 0);
  params.river_moisture = tb_get_num(world, str8_lit("river_moisture"), 0);
  params.plate_spacing = tb_get_num(world, str8_lit("plate_spacing"), 0);
  params.plate_fuzz = tb_get_num(world, str8_lit("plate_fuzz"), 0);
  params.plate_fuzz_scale = tb_get_num(world, str8_lit("plate_fuzz_scale"), 0);
  params.uplift_height = tb_get_num(world, str8_lit("uplift_height"), 0);
  params.uplift_width = tb_get_num(world, str8_lit("uplift_width"), 0);
  params.uplift_noise = tb_get_num(world, str8_lit("uplift_noise"), 0);
  params.uplift_noise_scale = tb_get_num(world, str8_lit("uplift_noise_scale"), 0);
  params.uplift_ridged = tb_get_num(world, str8_lit("uplift_ridged"), 0);
  params.uplift_ridged_scale = tb_get_num(world, str8_lit("uplift_ridged_scale"), 0);
  params.rift_depth = tb_get_num(world, str8_lit("rift_depth"), 0);
  params.rift_width = tb_get_num(world, str8_lit("rift_width"), 0);
  params.arc_height = tb_get_num(world, str8_lit("arc_height"), 0);
  params.continent_blend = tb_get_num(world, str8_lit("continent_blend"), 0);
  params.continent_height = tb_get_num(world, str8_lit("continent_height"), 0);
  params.min_region_size = (I32)tb_get_num(world, str8_lit("min_region_size"), 0);

  params.river_count = (I32)tb_get_num(world, str8_lit("river_count"), 0);
  params.river_max_tries = (I32)tb_get_num(world, str8_lit("river_max_tries"), 0);
  params.river_min_length = (I32)tb_get_num(world, str8_lit("river_min_length"), 0);
  params.river_meander = tb_get_num(world, str8_lit("river_meander"), 0);
  params.pond_epsilon = tb_get_num(world, str8_lit("pond_epsilon"), 0);
  params.pond_max_tiles = (I32)tb_get_num(world, str8_lit("pond_max_tiles"), 0);

  params.road_cost = tb_get_num(world, str8_lit("road_cost"), 0);
  params.river_cross_cost = tb_get_num(world, str8_lit("river_cross_cost"), 0);

  //- fp: the parameters whose value of 0 breaks the arithmetic, and does not
  //  only give a broken world. They are the divisors and the counts of
  //  octaves. An assert tests them, and no code changes them. A wrong value is
  //  a mistake in the file, and the person who wrote the file must correct it.
  //  Each other parameter gives a world that is visibly wrong on its own.
  Assert(1 <= params.width && params.width <= TH_WORLD_MAX_DIM);
  Assert(1 <= params.height && params.height <= TH_WORLD_MAX_DIM);
  Assert(params.elevation_scale > 0 && params.elevation_octaves > 0);
  Assert(params.moisture_scale > 0 && params.moisture_octaves > 0);
  Assert(params.temperature_scale > 0 && params.temperature_octaves > 0);
  Assert(params.plate_spacing > 0 && params.plate_fuzz_scale > 0);
  Assert(params.uplift_noise_scale > 0 && params.uplift_ridged_scale > 0);
  Assert(params.uplift_width > 0 && params.rift_width > 0);
  Assert(params.continent_blend > 0);
  arena_release_scratch(scratch);
  return params;
}

////////////////////////////////
//~ fp: Noise
//
// A deterministic noise from a hash of integers. A grid holds one hashed value
// at each point. The noise between two points is a smooth interpolation of
// those values, which is a value noise. The generator then adds the octaves,
// which is an fBm, and brings the sum back into [0,1].
//
// The hash reads the seed, and each octave changes the seed again. Two
// different fields, and two octaves of one field, are therefore independent,
// and no state for a random number generator is necessary.

internal F32 wg__value_noise(U64 seed, F32 x, F32 y) {
  F32 fx = floorf(x);
  F32 fy = floorf(y);
  I32 x0 = (I32)fx;
  I32 y0 = (I32)fy;
  F32 tx = x - fx;
  F32 ty = y - fy;
  tx = tx * tx * (3.0f - 2.0f * tx); // smoothstep: kills the lattice creases
  ty = ty * ty * (3.0f - 2.0f * ty);
  F32 n00 = rng_hash01_2d(seed, x0 + 0, y0 + 0);
  F32 n10 = rng_hash01_2d(seed, x0 + 1, y0 + 0);
  F32 n01 = rng_hash01_2d(seed, x0 + 0, y0 + 1);
  F32 n11 = rng_hash01_2d(seed, x0 + 1, y0 + 1);
  F32 nx0 = n00 + (n10 - n00) * tx;
  F32 nx1 = n01 + (n11 - n01) * tx;
  return nx0 + (nx1 - nx0) * ty;
}

internal F32 wg__fbm(U64 seed, F32 x, F32 y, I32 octaves, F32 persistence) {
  F32 sum = 0;
  F32 total = 0;
  F32 amplitude = 1.0f;
  for(I32 octave = 0; octave < octaves; octave += 1) {
    sum += amplitude * wg__value_noise(seed + (U64)octave * 0x9E3779B97F4A7C15ull, x, y);
    total += amplitude;
    amplitude *= persistence;
    x *= 2.0f;
    y *= 2.0f;
  }
  return sum / total;
}

// A ridged fBm. Each octave folds its noise around zero with 1 - |2n - 1|, and
// squares the result to make the fold sharp. The largest values therefore lie
// on the lines where the smooth noise crosses zero. The field has thin crests
// and narrow valleys, and not soft round shapes.
internal F32 wg__ridged_fbm(U64 seed, F32 x, F32 y, I32 octaves, F32 persistence) {
  F32 sum = 0;
  F32 total = 0;
  F32 amplitude = 1.0f;
  for(I32 octave = 0; octave < octaves; octave += 1) {
    F32 n = wg__value_noise(seed + (U64)octave * 0x9E3779B97F4A7C15ull, x, y);
    F32 folded = 1.0f - fabsf(2.0f * n - 1.0f);
    sum += amplitude * folded * folded;
    total += amplitude;
    amplitude *= persistence;
    x *= 2.0f;
    y *= 2.0f;
  }
  return sum / total;
}

////////////////////////////////
//~ fp: Tectonics
//
// The map divides into tectonic plates. Each plate is a voronoi cell with a
// rough border, around a seed point that a hash moves inside its grid cell.
// There is one seed for each cell of the size plate_spacing, so the seeds
// spread evenly, and their density is the same at any size of map.
//
// Each plate moves with a velocity that a hash gives it. Where the relative
// motion across a border presses two plates together, the tiles of that border
// make a ridge, and the compression is the strength of that ridge.
//
// The uplift then decreases with the distance to the nearest ridge tile. A
// chamfer pass computes that distance, which is close to euclidean. A noise
// moves the distance, so a range of mountains becomes narrow in places and
// turns. A ridged noise then shapes the result, so a crest breaks into peaks
// and passes and does not run as a smooth wall.
//
// The plates also shape the sea. Each plate that touches the border of the map
// falls into the rim of the continent, so a coast follows the borders of the
// plates. See wg__plate_fields.

#define WG__PLATE_SALT 0x6a09e667f3bcc909ull

internal V2 wg__plate_seed(WG_Params* params, U64 seed, I32 cx, I32 cy) {
  U64 salt = seed ^ WG__PLATE_SALT;
  V2 p;
  p.x = ((F32)cx + rng_hash01_2d(salt + 1, cx, cy)) * params->plate_spacing;
  p.y = ((F32)cy + rng_hash01_2d(salt + 2, cx, cy)) * params->plate_spacing;
  return p;
}

internal V2 wg__plate_velocity(U64 seed, I32 cx, I32 cy) {
  U64 salt = seed ^ WG__PLATE_SALT;
  F32 angle = 6.2831853f * rng_hash01_2d(salt + 3, cx, cy);
  F32 magnitude = rng_hash01_2d(salt + 4, cx, cy);
  return (V2){magnitude * cosf(angle), magnitude * sinf(angle)};
}

// The voronoi cell at a point, with a rough border. A noise moves the point
// before the search for the nearest seed, so a border turns. A plate_fuzz of 0
// gives an exact voronoi cell. There is one seed for each grid cell, so a scan
// of the two rings of cells around the moved point finds the true nearest
// seed.
internal I32 wg__plate_at(WG_Params* params, U64 seed, I32 gx, I32 gy, I32 x, I32 y) {
  U64 salt = seed ^ WG__PLATE_SALT;
  F32 fx = (F32)x;
  F32 fy = (F32)y;
  if(params->plate_fuzz > 0) {
    F32 s = params->plate_fuzz_scale;
    fx += params->plate_fuzz * (2.0f * wg__fbm(salt + 5, (F32)x / s, (F32)y / s, 2, 0.5f) - 1.0f);
    fy += params->plate_fuzz * (2.0f * wg__fbm(salt + 6, (F32)x / s, (F32)y / s, 2, 0.5f) - 1.0f);
  }
  I32 ccx = (I32)floorf(fx / params->plate_spacing);
  I32 ccy = (I32)floorf(fy / params->plate_spacing);
  I32 best = 0;
  F32 best_d2 = 1e30f;
  for(I32 cy = ClampBot(ccy - 2, 0); cy <= ClampTop(ccy + 2, gy - 1); cy += 1) {
    for(I32 cx = ClampBot(ccx - 2, 0); cx <= ClampTop(ccx + 2, gx - 1); cx += 1) {
      V2 p = wg__plate_seed(params, seed, cx, cy);
      F32 dx = fx - p.x;
      F32 dy = fy - p.y;
      F32 d2 = dx * dx + dy * dy;
      if(d2 < best_d2) {
        best_d2 = d2;
        best = cy * gx + cx;
      }
    }
  }
  return best;
}

// The convergence across the border between plate a and plate b. It is the
// relative velocity along the axis from a to b, brought into [-1,1]. Each
// velocity has a length of 1 at most, so the dot product lies in [-2,2].
//
// A positive value presses the two plates together and makes a ridge. A
// negative value pulls them apart and makes a rift or an ocean ridge. Two
// plates that slide past each other give a value near 0, and no relief.
internal F32 wg__plate_convergence(WG_Params* params, U64 seed, I32 gx, I32 a, I32 b) {
  V2 pa = wg__plate_seed(params, seed, a % gx, a / gx);
  V2 pb = wg__plate_seed(params, seed, b % gx, b / gx);
  V2 va = wg__plate_velocity(seed, a % gx, a / gx);
  V2 vb = wg__plate_velocity(seed, b % gx, b / gx);
  V2 axis = v2_norm(v2_sub(pb, pa), (V2){1, 0});
  F32 conv = (va.x - vb.x) * axis.x + (va.y - vb.y) * axis.y;
  return conv * 0.5f;
}

// The shape of a slope, which a ridge and a rift both use. A noise moves the
// distance, so the feature becomes narrow in places and turns. The value then
// falls with the square of the distance over `width`. A ridged noise then
// gives the result its texture.
internal F32 wg__flank_shape(WG_Params* params, U64 salt, F32 d, F32 width, I32 x, I32 y) {
  if(params->uplift_noise > 0) {
    F32 s = params->uplift_noise_scale;
    F32 wobble = wg__fbm(salt + 7, (F32)x / s, (F32)y / s, 2, 0.5f);
    d *= 1.0f + params->uplift_noise * (2.0f * wobble - 1.0f);
  }
  F32 t = Clamp(0.0f, d / width, 1.0f);
  F32 bell = (1.0f - t) * (1.0f - t);
  if(bell <= 0) { return 0; }
  F32 shape = 1.0f;
  if(params->uplift_ridged > 0) {
    F32 s = params->uplift_ridged_scale;
    F32 ridged = wg__ridged_fbm(salt + 8, (F32)x / s, (F32)y / s, 3, 0.5f);
    shape += params->uplift_ridged * (ridged - 1.0f); // lerp(1 .. ridged)
  }
  return bell * shape;
}

// A chamfer distance transform, in two passes, in place. A step to a side
// costs 1, and a step to a corner costs the square root of 2.
//
// At the start `dist` is 0 at each source cell and 1e9 at each other cell. At
// the end each cell holds a distance to the nearest source that is close to
// euclidean.
//
// `carry` can be absent. When it is present, each cell also ends with the
// value of its nearest source.
internal void wg__distance_transform(F32* dist, F32* carry, I32 w, I32 h) {
  for(I32 y = 0; y < h; y += 1) {
    for(I32 x = 0; x < w; x += 1) {
      U64 i = (U64)y * w + x;
      I32 ox[4] = {-1, -1, 0, 1};
      I32 oy[4] = {0, -1, -1, -1};
      F32 oc[4] = {1.0f, 1.41421356f, 1.0f, 1.41421356f};
      for(I32 k = 0; k < 4; k += 1) {
        I32 nx = x + ox[k];
        I32 ny = y + oy[k];
        if(nx < 0 || nx >= w || ny < 0 || ny >= h) { continue; }
        U64 j = (U64)ny * w + nx;
        if(dist[j] + oc[k] < dist[i]) {
          dist[i] = dist[j] + oc[k];
          if(carry != 0) { carry[i] = carry[j]; }
        }
      }
    }
  }
  for(I32 y = h - 1; y >= 0; y -= 1) {
    for(I32 x = w - 1; x >= 0; x -= 1) {
      U64 i = (U64)y * w + x;
      I32 ox[4] = {1, 1, 0, -1};
      I32 oy[4] = {0, 1, 1, 1};
      F32 oc[4] = {1.0f, 1.41421356f, 1.0f, 1.41421356f};
      for(I32 k = 0; k < 4; k += 1) {
        I32 nx = x + ox[k];
        I32 ny = y + oy[k];
        if(nx < 0 || nx >= w || ny < 0 || ny >= h) { continue; }
        U64 j = (U64)ny * w + nx;
        if(dist[j] + oc[k] < dist[i]) {
          dist[i] = dist[j] + oc[k];
          if(carry != 0) { carry[i] = carry[j]; }
        }
      }
    }
  }
}

// The whole set of plate steps. It writes one uplift value at each tile into
// out_uplift. It writes the size of the divergence into out_rift, as a value
// from 0 to 1 that no parameter has scaled. It writes the rim of the continent
// into out_rim, where 0 is under the sea and 1 is full ground.
internal void wg__plate_fields(WG_Params* params, U64 seed, F32* out_uplift,
                               F32* out_rift, F32* out_rim) {
  I32 w = params->width;
  I32 h = params->height;
  I32 gx = (I32)ceilf((F32)w / params->plate_spacing);
  I32 gy = (I32)ceilf((F32)h / params->plate_spacing);
  U64 salt = seed ^ WG__PLATE_SALT;
  ArenaTemp scratch = arena_get_scratch(0, 0);

  //- fp: give each tile its plate
  I32* plate = push_array_no_zero(scratch.arena, I32, (U64)w * h);
  for(I32 y = 0; y < h; y += 1) {
    for(I32 x = 0; x < w; x += 1) {
      plate[(U64)y * w + x] = wg__plate_at(params, seed, gx, gy, x, y);
    }
  }

  //- fp: make the ridges and the rifts. A test to the right and a test
  //  downward reach each inner border edge one time. Both sides of a border
  //  join the feature. A tile that touches two borders keeps the strongest
  //  press and the strongest pull.
  F32* dist = push_array_no_zero(scratch.arena, F32, (U64)w * h);
  F32* force = push_array_no_zero(scratch.arena, F32, (U64)w * h);
  F32* rdist = push_array_no_zero(scratch.arena, F32, (U64)w * h);
  F32* rforce = push_array_no_zero(scratch.arena, F32, (U64)w * h);
  for(U64 i = 0; i < (U64)w * h; i += 1) {
    dist[i] = 1e9f;
    force[i] = 0;
    rdist[i] = 1e9f;
    rforce[i] = 0;
  }
  for(I32 y = 0; y < h; y += 1) {
    for(I32 x = 0; x < w; x += 1) {
      U64 i = (U64)y * w + x;
      for(I32 k = 0; k < 2; k += 1) {
        I32 nx = k == 0 ? x + 1 : x;
        I32 ny = k == 0 ? y : y + 1;
        if(nx >= w || ny >= h) { continue; }
        U64 j = (U64)ny * w + nx;
        if(plate[i] == plate[j]) { continue; }
        F32 conv = wg__plate_convergence(params, seed, gx, plate[i], plate[j]);
        if(conv > 0) {
          dist[i] = 0;
          dist[j] = 0;
          force[i] = Max(force[i], conv);
          force[j] = Max(force[j], conv);
        }
        if(conv < 0) {
          rdist[i] = 0;
          rdist[j] = 0;
          rforce[i] = Max(rforce[i], -conv);
          rforce[j] = Max(rforce[j], -conv);
        }
      }
    }
  }

  wg__distance_transform(dist, force, w, h);
  wg__distance_transform(rdist, rforce, w, h);

  //- fp: the rim of the continent, which follows the plates. A plate that owns
  //  a tile at the border of the map is a sea floor, and its whole cell reads
  //  a rim of 0. The land then rises to full ground over continent_blend tiles
  //  inland of that area. A coast therefore follows the rough borders of the
  //  plates, and not the rectangle of the map. Each tile at the border of the
  //  map belongs to some plate, so the edge of the map is always water.
  U8* edge = push_array(scratch.arena, U8, (U64)gx * gy);
  for(I32 x = 0; x < w; x += 1) {
    edge[plate[x]] = 1;
    edge[plate[(U64)(h - 1) * w + x]] = 1;
  }
  for(I32 y = 0; y < h; y += 1) {
    edge[plate[(U64)y * w]] = 1;
    edge[plate[(U64)y * w + (w - 1)]] = 1;
  }
  F32* rim_dist = push_array_no_zero(scratch.arena, F32, (U64)w * h);
  for(U64 i = 0; i < (U64)w * h; i += 1) {
    rim_dist[i] = edge[plate[i]] ? 0.0f : 1e9f;
  }
  wg__distance_transform(rim_dist, 0, w, h);
  for(U64 i = 0; i < (U64)w * h; i += 1) {
    F32 t = ClampTop(rim_dist[i] / params->continent_blend, 1.0f);
    out_rim[i] = t * t * (3.0f - 2.0f * t);
  }

  //- fp: the sizes of the uplift and of the rift. Each one is a shaped fall
  //  that the force scales. The uplift is the elevation to add. The rift stays
  //  a size from 0 to 1 that no parameter has scaled, because wg__elevation_at
  //  gives it a meaning from the crust: rift_depth on land, and arc_height
  //  under the sea.
  for(I32 y = 0; y < h; y += 1) {
    for(I32 x = 0; x < w; x += 1) {
      U64 i = (U64)y * w + x;
      out_uplift[i] = 0;
      out_rift[i] = 0;
      if(force[i] > 0) {
        F32 m = wg__flank_shape(params, salt, dist[i], params->uplift_width, x, y);
        out_uplift[i] = params->uplift_height * force[i] * m;
      }
      if(rforce[i] > 0) {
        F32 m = wg__flank_shape(params, salt, rdist[i], params->rift_width, x, y);
        out_rift[i] = rforce[i] * m;
      }
    }
  }
  arena_release_scratch(scratch);
}

////////////////////////////////
//~ fp: Fields
//
// The elevation is an fBm and a tectonic uplift. The rim of the continent,
// which follows the plates, then sinks that sum. See Tectonics. The land
// therefore reads as a continent with natural coasts, and not as a pattern
// that repeats.

internal F32 wg__elevation_at(WG_Params* params, U64 seed, F32 uplift, F32 rift,
                              F32 rim, I32 x, I32 y) {
  F32 noise = wg__fbm(seed, (F32)x / params->elevation_scale,
                      (F32)y / params->elevation_scale, params->elevation_octaves,
                      params->elevation_persistence);
  // Three terms, added together. The first is the shelf that an inner plate
  // stands on. The second is a noise across [0, elevation_amplitude], which
  // shapes the relief. The third is the plate uplift, which raises the
  // mountains. The rim then scales the sum, and sinks it to the sea floor
  // across the plates at the border of the map.
  F32 e = params->continent_height + noise * params->elevation_amplitude + uplift;
  e = ClampTop(e, 1.0f) * rim;
  // The divergence takes its meaning from the crust, and the rim mixes the two
  // results. On land it sinks a rift valley, and the floor of that valley goes
  // below the sea in places and makes a line of lakes. Under the sea it raises
  // a ridge, which can reach the surface as a line of islands.
  e += rift * (params->arc_height * (1.0f - rim) - params->rift_depth * rim);
  return Clamp(0.0f, e, 1.0f);
}

// The moisture uses a different seed, so it is independent of the elevation.
#define WG__MOISTURE_SALT 0x8b3f9a1dcafeull

internal F32 wg__moisture_at(WG_Params* params, U64 seed, I32 x, I32 y) {
  return wg__fbm(seed ^ WG__MOISTURE_SALT, (F32)x / params->moisture_scale,
                 (F32)y / params->moisture_scale, params->moisture_octaves,
                 params->moisture_persistence);
}

#define WG__TEMPERATURE_SALT 0x2545f4914f6cdd1dull

// A gradient from the north to the equator to the south, which is linear
// between those three points. The equator is the middle row of the map. A
// noise then changes the value, and the height above the sea lowers it.
internal F32 wg__temperature_at(WG_Params* params, U64 seed, F32 e, I32 x, I32 y) {
  F32 lat = (F32)y / (F32)Max(params->height - 1, 1); // 0 north edge, 1 south edge
  F32 t = lat < 0.5f
              ? params->temperature_north + (params->temperature_equator - params->temperature_north) * (lat * 2.0f)
              : params->temperature_equator + (params->temperature_south - params->temperature_equator) * (lat * 2.0f - 1.0f);
  F32 wobble = wg__fbm(seed ^ WG__TEMPERATURE_SALT, (F32)x / params->temperature_scale,
                       (F32)y / params->temperature_scale, params->temperature_octaves,
                       params->temperature_persistence);
  t += params->temperature_variation * (wobble - 0.5f);
  // The fall with the height follows a curve. The exponent keeps the middle
  // ground warm and makes the high ridges cold, so the snow stays on the
  // crests, and it does so at a warm latitude too.
  F32 above = Max(e - params->sea_level, 0.0f) / Max(1.0f - params->sea_level, 0.01f);
  t -= params->temperature_lapse * powf(above, params->temperature_lapse_exponent);
  return Clamp(0.0f, t, 1.0f);
}

// How easily water leaves a tile, from 0 to 1. This field is not a noise of
// its own. It comes from the landscape that the earlier steps built. Steep
// ground sheds water, and the slope term measures that from the differences of
// the elevation field. High ground stands above the water table, and the
// height term measures that. Low flat land near the sea level therefore reads
// near 0, which is where a swamp belongs.
internal F32 wg__drainage_at(WG_Params* params, F32* elevation, I32 x, I32 y) {
  I32 w = params->width;
  I32 h = params->height;
  I32 xl = ClampBot(x - 1, 0), xr = ClampTop(x + 1, w - 1);
  I32 yu = ClampBot(y - 1, 0), yd = ClampTop(y + 1, h - 1);
  F32 dx = (elevation[(U64)y * w + xr] - elevation[(U64)y * w + xl]) / (F32)Max(xr - xl, 1);
  F32 dy = (elevation[(U64)yd * w + x] - elevation[(U64)yu * w + x]) / (F32)Max(yd - yu, 1);
  F32 slope = sqrtf(dx * dx + dy * dy);
  F32 slope_drain = ClampTop(slope / Max(params->drainage_full_slope, 0.001f), 1.0f);
  F32 e = elevation[(U64)y * w + x];
  F32 height_drain = Clamp(0.0f, (e - params->sea_level) / Max(params->drainage_ceiling - params->sea_level, 0.01f), 1.0f);
  return 0.6f * slope_drain + 0.4f * height_drain;
}

////////////////////////////////
//~ fp: Rivers
//
// A river starts at the highest of a set of sample points that a hash chooses.
// It then walks down the elevation field until it reaches water, or leaves the
// map at an edge, or stops in a basin. A river that stops in a basin fills
// that basin and makes a pond.
//
// The walk down does not always take the steepest step. Each neighbour that is
// lower is a candidate, and a value from a hash of the position chooses
// between them. A river therefore turns inside its valley, and does not run in
// a straight line.
//
// The hash reads the position, so two rivers that meet join into one system:
// from a shared tile they both choose the same way down. A connection is a
// mask, so a second write of the same connection changes nothing.

#define WG__RIVER_SALT        0x517cc1b727220a95ull
#define WG__MEANDER_SALT      0x2545f4914f6cdd1dull
#define WG__ELEVATION_OFF_MAP (-1000.0f)

internal void wg_field_connect(TH_Db* db, V2I p, Dir4 dir, TH_IField mask_field) {
  if(dir >= Dir4_COUNT) { return; }
  th_ifield_set_bit(db, p, mask_field, dir, true);
  th_ifield_set_bit(db, v2i_add(p, dir4_delta(dir)), mask_field, dir4_opposite(dir), true);
}

internal void wg__carve_rivers(TH_Db* db, WG_Params* params, U64 seed, F32* elevation) {
  I32 w = params->width;
  I32 h = params->height;
  // Each step goes to a lower tile, so a walk never reaches a tile two times.
  // w * h is therefore the exact limit of a walk.
  U64 max_steps = (U64)w * h;
  ArenaTemp scratch = arena_get_scratch(0, 0);
  //- fp: the trace buffer. The code walks a river first, and writes its
  //  connections after. A river that stops before river_min_length steps
  //  therefore leaves no mark on the map.
  V2I* trace_pos = push_array_no_zero(scratch.arena, V2I, max_steps);
  Dir4* trace_dir = push_array_no_zero(scratch.arena, Dir4, max_steps);
  V2I* pond_queue = push_array_no_zero(scratch.arena, V2I, (U64)params->pond_max_tiles);
  //- fp: the attempts, which chase a count. A river that is too short, and a
  //  source that the code cannot use, each cost one attempt and give no river.
  //  A map therefore reaches river_count, unless the terrain has no more room
  //  within river_max_tries.
  I32 carved = 0;
  for(I32 attempt = 0; attempt < params->river_max_tries && carved < params->river_count; attempt += 1) {
    //- fp: the source, which is the highest of 32 sample points on dry land
    //  that a hash chooses. The code rejects a sample that already carries a
    //  river. Each attempt therefore makes a new branch, and does not follow a
    //  channel that exists.
    V2I source = {0};
    F32 source_elevation = WG__ELEVATION_OFF_MAP;
    for(I32 sample = 0; sample < 32; sample += 1) {
      U64 salt = seed ^ WG__RIVER_SALT;
      V2I p = {(I32)(rng_hash_2d(salt, attempt, sample) % (U32)w),
               (I32)(rng_hash_2d(salt + 1, attempt, sample) % (U32)h)};
      F32 e = elevation[(U64)p.y * w + p.x];
      if(e > source_elevation && e >= params->sea_level &&
         th_ifield_get(db, p, TH_IField_RiverMask) == 0) {
        source = p;
        source_elevation = e;
      }
    }
    if(source_elevation <= WG__ELEVATION_OFF_MAP) { continue; } // no usable sample

    //- fp: walk down, and record each step. A candidate must be lower, which
    //  makes the walk stop. Among the candidates the code compares elevations
    //  that a hash changes a little, which makes the river turn.
    I32 length = 0;
    B32 in_basin = 0;
    V2I at = source;
    for(U64 step = 0; step < max_steps; step += 1) {
      F32 here = elevation[(U64)at.y * w + at.x];
      Dir4 down_dir = Dir4_COUNT;
      F32 down_score = 0;
      B32 down_off_map = 0;
      for(Dir4 dir = 0; dir < Dir4_COUNT; dir += 1) {
        V2I n = v2i_add(at, dir4_delta(dir));
        B32 off_map = !th_world_in_bounds(db, n);
        // A tile off the map is the lowest place. A spring near the border
        // therefore drains off the world, and does not make a pond.
        F32 e = off_map ? WG__ELEVATION_OFF_MAP : elevation[(U64)n.y * w + n.x];
        if(e >= here) { continue; } // only strictly downhill: no cycles
        F32 score = e + params->river_meander *
                            (rng_hash01_2d(seed ^ WG__MEANDER_SALT, n.x, n.y) - 0.5f);
        if(down_dir == Dir4_COUNT || score < down_score) {
          down_dir = dir;
          down_score = score;
          down_off_map = off_map;
        }
      }
      if(down_dir == Dir4_COUNT) {
        in_basin = 1;
        break;
      } // nowhere lower

      trace_pos[length] = at;
      trace_dir[length] = down_dir;
      length += 1;
      if(down_off_map) { break; }
      at = v2i_add(at, dir4_delta(down_dir));
      if(elevation[(U64)at.y * w + at.x] < params->sea_level) { break; } // reached the sea
    }

    if(length < params->river_min_length) { continue; } // stubby spring: cull it
    carved += 1;
    for(I32 idx = 0; idx < length; idx += 1) {
      wg_field_connect(db, trace_pos[idx], trace_dir[idx], TH_IField_RiverMask);
    }

    //- fp: A river ends in the water. The half of the connection at the mouth
    //  tile would therefore draw a short piece inside the sea. Clear that half
    //  only, and keep the other half: the tile on the land then flows up to
    //  the edge of the water. A river that leaves the map ends on land, so the
    //  test below skips it.
    if(!in_basin && elevation[(U64)at.y * w + at.x] < params->sea_level) {
      th_ifield_set(db, at, TH_IField_RiverMask, 0);
    }

    //- fp: a river that stops inland fills its basin and makes a pond. The
    //  connected land within pond_epsilon of the height of that basin sinks
    //  to just below the sea level, and pond_max_tiles limits how much land
    //  sinks. The classification then paints a lake there, its shores take the
    //  wetter values of a river bank, and a later river can end in it as in a
    //  sea. Each tile that sinks goes below sea_level, so the fill can never
    //  reach it two times.
    if(in_basin) {
      F32 basin_elevation = elevation[(U64)at.y * w + at.x];
      F32 pond = params->sea_level - 0.01f;
      elevation[(U64)at.y * w + at.x] = pond;
      th_ifield_set(db, at, TH_IField_RiverMask, 0); // no river inside the lake
      pond_queue[0] = at;
      I32 pond_count = 1;
      for(I32 head = 0; head < pond_count; head += 1) {
        for(Dir4 dir = 0; dir < Dir4_COUNT; dir += 1) {
          V2I n = v2i_add(pond_queue[head], dir4_delta(dir));
          if(pond_count >= params->pond_max_tiles) { break; }
          if(!th_world_in_bounds(db, n)) { continue; }
          F32 e = elevation[(U64)n.y * w + n.x];
          // The fill stops at a tile that holds water already.
          if(e >= params->sea_level && e <= basin_elevation + params->pond_epsilon) {
            elevation[(U64)n.y * w + n.x] = pond;
            th_ifield_set(db, n, TH_IField_RiverMask, 0); // drowns any channel here
            pond_queue[pond_count] = n;
            pond_count += 1;
          }
        }
      }
    }
  }
  arena_release_scratch(scratch);
}

internal F32* wg__elevation_field(Arena* arena, U64 seed, WG_Params* params) {
  I32 w = params->width;
  I32 h = params->height;
  ArenaTemp scratch = arena_get_scratch(&arena, 1);
  F32* uplift = push_array_no_zero(scratch.arena, F32, (U64)w * h);
  F32* rift = push_array_no_zero(scratch.arena, F32, (U64)w * h);
  F32* rim = push_array_no_zero(scratch.arena, F32, (U64)w * h);
  wg__plate_fields(params, seed, uplift, rift, rim);
  F32* elevation = push_array_no_zero(arena, F32, (U64)w * h);
  for(I32 y = 0; y < h; y += 1) {
    for(I32 x = 0; x < w; x += 1) {
      U64 i = (U64)y * w + x;
      elevation[i] = wg__elevation_at(params, seed, uplift[i], rift[i], rim[i], x, y);
    }
  }
  arena_release_scratch(scratch);
  return elevation;
}

internal U8* wg__determine_sea(Arena* arena, WG_Params* params, F32* elevation) {
  I32 w = params->width;
  I32 h = params->height;
  ArenaTemp scratch = arena_get_scratch(&arena, 1);
  U8* sea = push_array(arena, U8, (U64)w * h);
  {
    V2I* queue = push_array_no_zero(scratch.arena, V2I, (U64)w * h);
    I32 count = 0;
    for(I32 y = 0; y < h; y += 1) {
      for(I32 x = 0; x < w; x += 1) {
        if(x != 0 && y != 0 && x != w - 1 && y != h - 1) { continue; }
        if(elevation[(U64)y * w + x] >= params->sea_level) { continue; }
        if(sea[(U64)y * w + x]) { continue; }
        sea[(U64)y * w + x] = 1;
        queue[count] = (V2I){x, y};
        count += 1;
      }
    }
    for(I32 head = 0; head < count; head += 1) {
      for(Dir4 dir = 0; dir < Dir4_COUNT; dir += 1) {
        V2I n = v2i_add(queue[head], dir4_delta(dir));
        if(n.x < 0 || n.x >= w || n.y < 0 || n.y >= h || sea[(U64)n.y * w + n.x]) { continue; }
        if(elevation[(U64)n.y * w + n.x] >= params->sea_level) { continue; }
        sea[(U64)n.y * w + n.x] = 1;
        queue[count] = n;
        count += 1;
      }
    }
  }
  arena_release_scratch(scratch);
  return sea;
}

internal void wg__classify_tiles(WG_Params* params, U64 seed, TH_Db* db, F32* elevation, U8* sea) {
  //- fp: classify the tiles. Compute the fields, then take the first terrain
  //  row that matches. See WG_TerrainType. A river bank reads wetter than its
  //  moisture field, so a valley becomes green or a swamp.
  I32 w = params->width;
  I32 h = params->height;
  for(I32 y = 0; y < h; y += 1) {
    for(I32 x = 0; x < w; x += 1) {
      V2I p = {x, y};
      F32 e = elevation[(U64)y * w + x];
      F32 moisture = wg__moisture_at(params, seed, x, y);
      F32 drainage = wg__drainage_at(params, elevation, x, y);
      F32 temperature = wg__temperature_at(params, seed, e, x, y);

      B32 river_nearby = th_ifield_get(db, p, TH_IField_RiverMask) != 0;
      for(Dir4 dir = 0; !river_nearby && dir < Dir4_COUNT; dir += 1) {
        river_nearby = th_ifield_get(db, v2i_add(p, dir4_delta(dir)), TH_IField_RiverMask) != 0;
      }
      if(river_nearby) { moisture = ClampTop(moisture + params->river_moisture, 1.0f); }

      B32 coast = 0;
      for(I32 dy = -1; !coast && dy <= 1; dy += 1) {
        for(I32 dx = -1; !coast && dx <= 1; dx += 1) {
          I32 nx = Clamp(0, x + dx, w - 1);
          I32 ny = Clamp(0, y + dy, h - 1);
          coast = sea[(U64)ny * w + nx] != 0;
        }
      }

      th_ifield_set(db, p, TH_IField_Terrain,
                    (I32)wg__classify(e, moisture, drainage, temperature, coast));
    }
  }
}

internal void wg__despeckle(WG_Params* params, TH_Db* db) {
  I32 w = params->width;
  I32 h = params->height;
  ArenaTemp scratch = arena_get_scratch(0, 0);
  if(params->min_region_size > 1) {
    U8* visited = push_array_no_zero(scratch.arena, U8, (U64)w * h);
    V2I* queue = push_array_no_zero(scratch.arena, V2I, (U64)w * h);
    for(I32 pass = 0; pass < 8; pass += 1) {
      B32 merged = 0;
      MemoryZero(visited, (U64)w * h);
      for(I32 y = 0; y < h; y += 1) {
        for(I32 x = 0; x < w; x += 1) {
          if(visited[(U64)y * w + x]) { continue; }
          I32 terrain = th_ifield_get(db, (V2I){x, y}, TH_IField_Terrain);
          visited[(U64)y * w + x] = 1;
          queue[0] = (V2I){x, y};
          I32 size = 1;
          for(I32 head = 0; head < size; head += 1) {
            for(Dir4 dir = 0; dir < Dir4_COUNT; dir += 1) {
              V2I n = v2i_add(queue[head], dir4_delta(dir));
              if(!th_world_in_bounds(db, n) || visited[(U64)n.y * w + n.x]) { continue; }
              if(th_ifield_get(db, n, TH_IField_Terrain) != terrain) { continue; }
              visited[(U64)n.y * w + n.x] = 1;
              queue[size] = n;
              size += 1;
            }
          }
          if(size >= params->min_region_size) { continue; }
          U32 votes[WG_TERRAIN_CAP] = {0};
          for(I32 i = 0; i < size; i += 1) {
            for(Dir4 dir = 0; dir < Dir4_COUNT; dir += 1) {
              V2I n = v2i_add(queue[i], dir4_delta(dir));
              if(!th_world_in_bounds(db, n)) { continue; }
              I32 nt = th_ifield_get(db, n, TH_IField_Terrain);
              if(nt != terrain) { votes[nt] += 1; }
            }
          }
          U32 best = (U32)terrain;
          U32 best_votes = 0;
          for(U32 i = 0; i < WG_TERRAIN_TYPE_COUNT; i += 1) {
            if(votes[i] > best_votes) {
              best = i;
              best_votes = votes[i];
            }
          }
          if(best_votes == 0) { continue; } // the whole map is one small region
          for(I32 i = 0; i < size; i += 1) {
            th_ifield_set(db, queue[i], TH_IField_Terrain, (I32)best);
          }
          merged = 1;
        }
      }
      if(!merged) { break; }
    }
  }
  arena_release_scratch(scratch);
}

////////////////////////////////
//~ fp: Generation

internal void wg_generate(TH_Db* db, WG_Params* params, U64 seed) {
  Assert(params->width <= TH_WORLD_MAX_DIM && params->height <= TH_WORLD_MAX_DIM);
  th_world_size_set(db, params->width, params->height);

  //- fp: generation owns each field of the world. It writes over the terrain,
  //  the rivers and the roads that the fields held, and keeps none of them.
  for(I32 y = 0; y < params->height; y += 1) {
    for(I32 x = 0; x < params->width; x += 1) {
      V2I p = {x, y};
      th_ifield_set(db, p, TH_IField_Terrain, 0);
      th_ifield_set(db, p, TH_IField_RiverMask, 0);
      th_ifield_set(db, p, TH_IField_RoadMask, 0);
    }
  }

  ArenaTemp scratch = arena_get_scratch(0, 0);

  //- fp: the elevation field, which lives for the whole generation. The
  //  classification and the rivers must agree on the way down. The tectonic
  //  uplift comes first, because each later step reads the elevation: the
  //  rivers do, and the line of the snow does.
  F32* elevation = wg__elevation_field(scratch.arena, seed, params);

  //- fp: the rivers come first. The classification reads them, so a river
  //  valley becomes wetter than the rain alone would make it.
  wg__carve_rivers(db, params, seed, elevation);

  //- fp: the sea is the water below sea_level that joins the border of the
  //  map. The rim makes each tile at that border a sea tile. A lake and a pond
  //  are also below sea_level, but they do not join the border, so their
  //  shores never read as a coast. A beach belongs to the sea only.
  U8* sea = wg__determine_sea(scratch.arena, params, elevation);

  //- fp: classify the tiles. Compute the fields, then take the first terrain
  //  row that matches. See WG_TerrainType. A river bank reads wetter than its
  //  moisture field, so a valley becomes green or a swamp.
  wg__classify_tiles(params, seed, db, elevation, sea);

  //- fp: remove the small regions. A region of terrain that is smaller than
  //  min_region_size becomes the terrain of its most common neighbour. The
  //  step repeats until the map stops to change.
  wg__despeckle(params, db);

  arena_release_scratch(scratch);
}

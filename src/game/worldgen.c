#include "base/core.h"
#include "base/math.h"
#include "base/arena.h"
#include "base/strings.h"
#include "base/print.h"
#include "base/tctx.h"
#include "tabula.h"
#include "game/board.h"
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

internal V4 wg__color_from_key(TB_Value* object, String8 key, V4 fallback) {
  V4 color = fallback;
  TB_Value* list = tb_get(object, key);
  if(list->kind == TB_ValueKind_List && list->count >= 3) {
    color.x = tb_num_from_value(list->first, 0);
    color.y = tb_num_from_value(list->first->next, 0);
    color.z = tb_num_from_value(list->first->next->next, 0);
    color.w = 1;
  }
  return color;
}

internal B32 wg__band_contains(WG_Band band, F32 v) {
  if(band.min == 0 && band.max == 0) { return 1; } // zero band = don't care
  return band.min <= v && v <= band.max;
}

// first row (past nil) whose bands all contain the fields; 0 = no row claims it
internal U32 wg__classify(WG_Params* params, F32 e, F32 moisture, F32 drainage,
                          F32 temperature, B32 coast) {
  for(U32 i = 1; i < params->terrain_count; i += 1) {
    WG_TerrainDef* def = &params->terrains[i];
    if(!wg__band_contains(def->elevation, e)) { continue; }
    if(!wg__band_contains(def->moisture, moisture)) { continue; }
    if(!wg__band_contains(def->drainage, drainage)) { continue; }
    if(!wg__band_contains(def->temperature, temperature)) { continue; }
    if(def->needs_coast && !coast) { continue; }
    return i;
  }
  return 0;
}

// Bands are axis-aligned boxes, so any coverage gap contains the midpoint of
// some cell in the grid of all band boundaries: testing those midpoints is an
// exact check. Gaps report to stderr and classify as nil (loud magenta).
internal void wg__report_band_gaps(String8 path, WG_Params* params) {
  F32 cuts[4][2 * WG_TERRAIN_CAP + 2];
  U32 cut_counts[4];
  for(U32 dim = 0; dim < 4; dim += 1) {
    cuts[dim][0] = 0;
    cuts[dim][1] = 1;
    cut_counts[dim] = 2;
  }
  for(U32 i = 1; i < params->terrain_count; i += 1) {
    WG_TerrainDef* def = &params->terrains[i];
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
          if(wg__classify(params, e, m, d, t, 0) == 0) {
            eprintf_str8("%S: no terrain matches elevation %.2f moisture %.2f drainage %.2f temperature %.2f\n",
                         path, e, m, d, t);
            gaps += 1;
          }
        }
      }
    }
  }
}

internal WG_Params wg_params_load(Arena* arena, String8 path) {
  ArenaTemp scratch = arena_get_scratch(&arena, 1); // names push onto `arena`; keep it out of scratch
  TB_Value* root = tb_parse_file_and_report(scratch.arena, path);
  TB_Value* world = tb_get(root, str8_lit("world"));

  // all fallbacks are zero on purpose: a missing or misspelled key must
  // produce an obviously broken world, not a quietly substituted default --
  // the file is the single source of truth
  WG_Params params = {0};
  params.width = (I32)tb_get_num(world, str8_lit("width"), 0);
  params.height = (I32)tb_get_num(world, str8_lit("height"), 0);

  params.elevation_scale = tb_get_num(world, str8_lit("elevation_scale"), 0);
  params.elevation_octaves = (I32)tb_get_num(world, str8_lit("elevation_octaves"), 0);
  params.elevation_persistence = tb_get_num(world, str8_lit("elevation_persistence"), 0);
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
  params.continent_edge = tb_get_num(world, str8_lit("continent_edge"), 0);
  params.continent_smoothness = tb_get_num(world, str8_lit("continent_smoothness"), 0);
  params.min_region_size = (I32)tb_get_num(world, str8_lit("min_region_size"), 0);
  params.ridge_count = (I32)tb_get_num(world, str8_lit("ridge_count"), 0);
  params.ridge_height = tb_get_num(world, str8_lit("ridge_height"), 0);
  params.ridge_width = tb_get_num(world, str8_lit("ridge_width"), 0);
  params.ridge_wander = tb_get_num(world, str8_lit("ridge_wander"), 0);
  params.ridge_min_length = tb_get_num(world, str8_lit("ridge_min_length"), 0);
  params.elevation_amplitude = tb_get_num(world, str8_lit("elevation_amplitude"), 0);

  //- fp: terrain rows, in file order; row 0 is the baked nil
  {
    WG_TerrainDef* nil_def = &params.terrains[0];
    nil_def->name = str8_lit("nil");
    nil_def->color = (V4){1, 0, 1, 1}; // loud magenta
    params.terrain_count = 1;
  }
  for(TB_Node* node = world->first_member; node != 0; node = node->next) {
    if(!str8_match(node->key, str8_lit("terrain_type"), 0)) { continue; }
    if(params.terrain_count >= WG_TERRAIN_CAP) {
      eprintf_str8("%S: more than %d terrain_type entries; extras ignored\n",
                   path, WG_TERRAIN_CAP - 1);
      break;
    }
    TB_Value* src = &node->value;
    WG_TerrainDef* def = &params.terrains[params.terrain_count];
    params.terrain_count += 1;

    def->name = push_str8_copy(arena, tb_get_str8(src, str8_lit("name"), str8_lit("unnamed")));
    def->color = wg__color_from_key(src, str8_lit("color"), (V4){1, 0, 1, 1}); // magenta = the loud fallback
    def->rank = (U8)tb_get_num(src, str8_lit("rank"), 0);
    def->overlay_density = (U32)tb_get_num(src, str8_lit("overlay_density"), 0);
    def->move_cost = tb_get_num(src, str8_lit("move_cost"), 0); // missing = impassable, visibly

    def->elevation = wg__band_from_key(src, str8_lit("elevation"));
    def->moisture = wg__band_from_key(src, str8_lit("moisture"));
    def->drainage = wg__band_from_key(src, str8_lit("drainage"));
    def->temperature = wg__band_from_key(src, str8_lit("temperature"));
    def->needs_coast = tb_get_num(src, str8_lit("needs_coast"), 0) != 0;
  }
  wg__report_band_gaps(path, &params);

  params.river_count = (I32)tb_get_num(world, str8_lit("river_count"), 0);
  params.river_max_tries = (I32)tb_get_num(world, str8_lit("river_max_tries"), 0);
  params.river_min_length = (I32)tb_get_num(world, str8_lit("river_min_length"), 0);
  params.river_meander = tb_get_num(world, str8_lit("river_meander"), 0);
  params.pond_epsilon = tb_get_num(world, str8_lit("pond_epsilon"), 0);
  params.pond_max_tiles = (I32)tb_get_num(world, str8_lit("pond_max_tiles"), 0);

  params.road_cost = tb_get_num(world, str8_lit("road_cost"), 0);
  params.river_cross_cost = tb_get_num(world, str8_lit("river_cross_cost"), 0);

  //- fp: keep downstream code out of degenerate territory, loudly
  if(params.width < 1 || params.height < 1) {
    eprintf_str8("%S: bad world dimensions %dx%d, using 256x256\n",
                 path, params.width, params.height);
    params.width = 256;
    params.height = 256;
  }
  params.elevation_scale = ClampBot(params.elevation_scale, 1.0f);
  params.moisture_scale = ClampBot(params.moisture_scale, 1.0f);
  params.elevation_octaves = Clamp(1, params.elevation_octaves, 16);
  params.moisture_octaves = Clamp(1, params.moisture_octaves, 16);
  params.elevation_persistence = Clamp(0.05f, params.elevation_persistence, 1.0f);
  params.moisture_persistence = Clamp(0.05f, params.moisture_persistence, 1.0f);
  params.temperature_scale = ClampBot(params.temperature_scale, 1.0f);
  params.temperature_octaves = Clamp(1, params.temperature_octaves, 16);
  params.temperature_persistence = Clamp(0.05f, params.temperature_persistence, 1.0f);
  params.temperature_lapse_exponent = ClampBot(params.temperature_lapse_exponent, 0.1f);
  params.continent_edge = Clamp(0.0f, params.continent_edge, 1.0f);
  // the blend must complete before the border, or the border isn't water
  params.continent_smoothness = Clamp(0.001f, params.continent_smoothness,
                                      Max(params.continent_edge, 0.001f));
  params.ridge_count = Clamp(0, params.ridge_count, 64);
  params.river_min_length = ClampBot(params.river_min_length, 0);
  params.river_max_tries = ClampBot(params.river_max_tries, 0);
  params.pond_max_tiles = ClampBot(params.pond_max_tiles, 1);
  arena_release_scratch(scratch);
  return params;
}

////////////////////////////////
//~ fp: Noise
//
// Deterministic integer-hash noise: a lattice of hashed values, smoothstep-
// interpolated (value noise), summed over octaves (fBm, normalized back to
// [0,1]). The seed is folded into the hash, and each octave re-salts it, so
// distinct fields and octaves decorrelate without any rng state.

internal U32 wg__hash(U64 seed, I32 x, I32 y) {
  U32 h = (U32)(seed ^ (seed >> 32));
  h += (U32)x * 374761393u + (U32)y * 668265263u;
  h ^= h >> 16;
  h *= 0x7feb352du;
  h ^= h >> 15;
  h *= 0x846ca68bu;
  h ^= h >> 16;
  return h;
}

internal F32 wg__noise01(U64 seed, I32 x, I32 y) {
  return (F32)wg__hash(seed, x, y) * (1.0f / 4294967296.0f);
}

internal F32 wg__value_noise(U64 seed, F32 x, F32 y) {
  F32 fx = floorf(x);
  F32 fy = floorf(y);
  I32 x0 = (I32)fx;
  I32 y0 = (I32)fy;
  F32 tx = x - fx;
  F32 ty = y - fy;
  tx = tx * tx * (3.0f - 2.0f * tx); // smoothstep: kills the lattice creases
  ty = ty * ty * (3.0f - 2.0f * ty);
  F32 n00 = wg__noise01(seed, x0 + 0, y0 + 0);
  F32 n10 = wg__noise01(seed, x0 + 1, y0 + 0);
  F32 n01 = wg__noise01(seed, x0 + 0, y0 + 1);
  F32 n11 = wg__noise01(seed, x0 + 1, y0 + 1);
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

////////////////////////////////
//~ fp: Fields
//
// Elevation is fBm minus a radial falloff toward the map edge, so the border
// tends to ocean and the land reads as a continent rather than wallpaper.

#define WG__RIDGE_SALT 0x9e2f8c1b5a7d3e41ull
#define WG__RIDGE_SAMPLES 64

// tectonic ridge skeletons: jittered polylines between distant points,
// sampled into out_points (WG__RIDGE_SAMPLES per ridge). Endpoints land
// anywhere on the map: the rim drowns whatever crosses it, so a ridge can
// run out to sea and surface as island arcs.
internal I32 wg__build_ridges(WG_Params* params, U64 seed, V2* out_points) {
  I32 count = 0;
  F32 w = (F32)params->width;
  F32 h = (F32)params->height;
  for(I32 ridge = 0; ridge < params->ridge_count; ridge += 1) {
    U64 salt = seed ^ WG__RIDGE_SALT;
    V2 a = {0};
    V2 b = {0};
    // keep the longest pair seen: an unachievable minimum degrades to "as
    // long as the map allows"
    F32 best_d2 = -1.0f;
    F32 min_len = params->ridge_min_length * Min(w, h);
    for(I32 attempt = 0; attempt < 16; attempt += 1) {
      V2 ca, cb;
      ca.x = w * wg__noise01(salt + 1, ridge, attempt);
      ca.y = h * wg__noise01(salt + 2, ridge, attempt);
      cb.x = w * wg__noise01(salt + 3, ridge, attempt);
      cb.y = h * wg__noise01(salt + 4, ridge, attempt);
      F32 dx = cb.x - ca.x;
      F32 dy = cb.y - ca.y;
      F32 d2 = dx * dx + dy * dy;
      if(d2 > best_d2) {
        best_d2 = d2;
        a = ca;
        b = cb;
      }
      if(d2 >= min_len * min_len) { break; }
    }
    F32 dx = b.x - a.x;
    F32 dy = b.y - a.y;
    F32 len = sqrtf(dx * dx + dy * dy);
    F32 px = -dy / Max(len, 0.001f); // unit perpendicular carries the wander
    F32 py = dx / Max(len, 0.001f);
    for(I32 s = 0; s < WG__RIDGE_SAMPLES; s += 1) {
      F32 t = (F32)s / (F32)(WG__RIDGE_SAMPLES - 1);
      // smooth 1d noise along the spine; the sine pins wander at the ends
      F32 wander = params->ridge_wander * sinf(3.14159265f * t) *
                   (2.0f * wg__value_noise(salt + 5, t * 6.0f, (F32)ridge * 17.0f) - 1.0f);
      out_points[count].x = a.x + dx * t + px * wander;
      out_points[count].y = a.y + dy * t + py * wander;
      count += 1;
    }
  }
  return count;
}

internal F32 wg__elevation_at(WG_Params* params, U64 seed, V2* ridge_points,
                              I32 ridge_point_count, I32 x, I32 y) {
  F32 noise = wg__fbm(seed, (F32)x / params->elevation_scale,
                      (F32)y / params->elevation_scale, params->elevation_octaves,
                      params->elevation_persistence);
  // noise deviates around the midline by elevation_amplitude, leaving the
  // ridges as the mountain-maker while noise shapes flanks and passes
  F32 e = 0.5f + (noise - 0.5f) * params->elevation_amplitude;
  if(ridge_point_count > 0) {
    F32 best = 1e9f;
    for(I32 i = 0; i < ridge_point_count; i += 1) {
      F32 dx = (F32)x - ridge_points[i].x;
      F32 dy = (F32)y - ridge_points[i].y;
      F32 d2 = dx * dx + dy * dy;
      if(d2 < best) { best = d2; }
    }
    F32 u = sqrtf(best) / Max(params->ridge_width, 0.5f);
    if(u < 1.0f) {
      F32 bell = 1.0f - u * u;
      e += params->ridge_height * bell * bell;
    }
  }
  F32 nx = 2.0f * (F32)x / (F32)Max(params->width - 1, 1) - 1.0f; // [-1,1] across the map
  F32 ny = 2.0f * (F32)y / (F32)Max(params->height - 1, 1) - 1.0f;
  // the continental rim: inside continent_edge the heightmap stands as
  // tuned; from there elevation interpolates to 0 (deep water) over
  // continent_smoothness of border distance, so the border is always ocean
  F32 edge_dist = 1.0f - Max(Max(nx, -nx), Max(ny, -ny)); // 0 border, 1 center
  if(edge_dist < params->continent_edge) {
    F32 t = Clamp(0.0f, (params->continent_edge - edge_dist) / params->continent_smoothness, 1.0f);
    t = t * t * (3.0f - 2.0f * t);
    e *= 1.0f - t;
  }
  return e;
}

// moisture decorrelates from elevation by salting the seed
#define WG__MOISTURE_SALT 0x8b3f9a1dcafeull

internal F32 wg__moisture_at(WG_Params* params, U64 seed, I32 x, I32 y) {
  return wg__fbm(seed ^ WG__MOISTURE_SALT, (F32)x / params->moisture_scale,
                 (F32)y / params->moisture_scale, params->moisture_octaves,
                 params->moisture_persistence);
}

#define WG__TEMPERATURE_SALT 0x2545f4914f6cdd1dull

// a north/equator/south latitude gradient (piecewise-linear; the equator is
// the map's middle row), wobbled by noise, cooled by altitude above the sea
internal F32 wg__temperature_at(WG_Params* params, U64 seed, F32 e, I32 x, I32 y) {
  F32 lat = (F32)y / (F32)Max(params->height - 1, 1); // 0 north edge, 1 south edge
  F32 t = lat < 0.5f
              ? params->temperature_north + (params->temperature_equator - params->temperature_north) * (lat * 2.0f)
              : params->temperature_equator + (params->temperature_south - params->temperature_equator) * (lat * 2.0f - 1.0f);
  F32 wobble = wg__fbm(seed ^ WG__TEMPERATURE_SALT, (F32)x / params->temperature_scale,
                       (F32)y / params->temperature_scale, params->temperature_octaves,
                       params->temperature_persistence);
  t += params->temperature_variation * (wobble - 0.5f);
  // altitude cooling curves with height: the exponent spares mid ground and
  // freezes ridgelines sharply, so snow pins to spines even in warm latitudes
  F32 above = Max(e - params->sea_level, 0.0f) / Max(1.0f - params->sea_level, 0.01f);
  t -= params->temperature_lapse * powf(above, params->temperature_lapse_exponent);
  return Clamp(0.0f, t, 1.0f);
}

// How readily water leaves a tile, in [0,1]. Not a noise field of its own:
// drainage is read off the landscape already built -- steep ground sheds
// water (slope term, central differences over the elevation field), high
// ground stands above the water table (height term). Low flat land near sea
// level scores near 0, which is exactly where swamps belong.
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
// Each river starts at the highest of a handful of hashed sample points and
// walks downhill over the elevation field until it reaches water, flows off
// the map edge, or bottoms out in a basin -- which it floods into a pond.
// Descent is not pure steepest: any strictly lower neighbor is eligible, and
// a hashed jitter keyed on position picks the winner, so rivers meander
// through their valleys instead of ruling straight lines. Position-keyed
// jitter also means rivers that meet merge into tributary systems: from a
// shared tile they choose the same way down, and connections are a mask, so
// re-connecting is idempotent.

#define WG__RIVER_SALT        0x517cc1b727220a95ull
#define WG__MEANDER_SALT      0x2545f4914f6cdd1dull
#define WG__ELEVATION_OFF_MAP (-1000.0f)

internal void wg__carve_rivers(BD_Board* board, WG_Params* params, U64 seed, F32* elevation) {
  I32 w = board->width;
  I32 h = board->height;
  // strict descent can never revisit a tile, so w * h bounds any walk exactly
  U64 max_steps = (U64)w * h;
  ArenaTemp scratch = arena_get_scratch(0, 0);
  //- fp: trace buffer: a river is walked first and connected after, so one
  //  that peters out under river_min_length steps is culled, not drawn
  V2I* trace_pos = push_array_no_zero(scratch.arena, V2I, max_steps);
  BD_Dir* trace_dir = push_array_no_zero(scratch.arena, BD_Dir, max_steps);
  V2I* pond_queue = push_array_no_zero(scratch.arena, V2I, (U64)params->pond_max_tiles);
  //- fp: attempts chase a quota: a culled stub or dead source costs a try,
  //  not a river, so maps reach river_count unless the terrain truly runs
  //  out within river_max_tries
  I32 carved = 0;
  for(I32 attempt = 0; attempt < params->river_max_tries && carved < params->river_count; attempt += 1) {
    //- fp: source: highest of 32 hashed samples on dry land; samples already
    //  carrying a river are rejected, so every attempt buys a new tributary
    //  instead of retracing an existing channel
    V2I source = {0};
    F32 source_elevation = WG__ELEVATION_OFF_MAP;
    for(I32 sample = 0; sample < 32; sample += 1) {
      U64 salt = seed ^ WG__RIVER_SALT;
      V2I p = {(I32)(wg__hash(salt, attempt, sample) % (U32)w),
               (I32)(wg__hash(salt + 1, attempt, sample) % (U32)h)};
      F32 e = elevation[(U64)p.y * w + p.x];
      if(e > source_elevation && e >= params->sea_level &&
         bd_feature_mask(board, p, BD_Feature_River) == 0) {
        source = p;
        source_elevation = e;
      }
    }
    if(source_elevation <= WG__ELEVATION_OFF_MAP) { continue; } // no usable sample

    //- fp: descend, tracing. Eligibility is strictly-lower (termination);
    //  preference among the eligible is jittered elevation (meander)
    I32 length = 0;
    B32 in_basin = 0;
    V2I at = source;
    for(U64 step = 0; step < max_steps; step += 1) {
      F32 here = elevation[(U64)at.y * w + at.x];
      BD_Dir down_dir = BD_Dir_COUNT;
      F32 down_score = 0;
      B32 down_off_map = 0;
      for(BD_Dir dir = 0; dir < BD_Dir_COUNT; dir += 1) {
        V2I n = v2i_add(at, bd_dir_delta(dir));
        B32 off_map = !bd_in_bounds(board, n);
        // off the map counts as the lowest place there is: coastal springs
        // near the border drain off the world instead of pooling
        F32 e = off_map ? WG__ELEVATION_OFF_MAP : elevation[(U64)n.y * w + n.x];
        if(e >= here) { continue; } // only strictly downhill: no cycles
        F32 score = e + params->river_meander *
                        (wg__noise01(seed ^ WG__MEANDER_SALT, n.x, n.y) - 0.5f);
        if(down_dir == BD_Dir_COUNT || score < down_score) {
          down_dir = dir;
          down_score = score;
          down_off_map = off_map;
        }
      }
      if(down_dir == BD_Dir_COUNT) { in_basin = 1; break; } // nowhere lower

      trace_pos[length] = at;
      trace_dir[length] = down_dir;
      length += 1;
      if(down_off_map) { break; }
      at = v2i_add(at, bd_dir_delta(down_dir));
      if(elevation[(U64)at.y * w + at.x] < params->sea_level) { break; } // reached the sea
    }

    if(length < params->river_min_length) { continue; } // stubby spring: cull it
    carved += 1;
    for(I32 idx = 0; idx < length; idx += 1) {
      bd_feature_connect(board, trace_pos[idx], trace_dir[idx], BD_Feature_River);
    }

    //- fp: a river ends *in* water, so the mouth tile's mirrored half would
    //  draw a stub inside the sea: clear it, one-sidedly on purpose -- the
    //  land neighbor keeps its half and flows right up to the water's edge.
    //  (An off-map ending leaves `at` on land, so the condition skips it.)
    if(!in_basin && elevation[(U64)at.y * w + at.x] < params->sea_level) {
      bd_tile_at(board, at)->features[BD_Feature_River] = 0;
    }

    //- fp: a river that dies inland floods its basin into a pond: the
    //  connected bowl of land within pond_epsilon of the basin's height
    //  sinks just below sea level (bounded by pond_max_tiles), so
    //  classification paints a lake there, its shores get the riverbank
    //  treatment, and later rivers can end in it like a sea. Flooded tiles
    //  drop below sea_level, so the fill can never revisit them.
    if(in_basin) {
      F32 basin_elevation = elevation[(U64)at.y * w + at.x];
      F32 pond = params->sea_level - 0.01f;
      elevation[(U64)at.y * w + at.x] = pond;
      bd_tile_at(board, at)->features[BD_Feature_River] = 0; // no river inside the lake
      pond_queue[0] = at;
      I32 pond_count = 1;
      for(I32 head = 0; head < pond_count; head += 1) {
        for(BD_Dir dir = 0; dir < BD_Dir_COUNT; dir += 1) {
          V2I n = v2i_add(pond_queue[head], bd_dir_delta(dir));
          if(pond_count >= params->pond_max_tiles) { break; }
          if(!bd_in_bounds(board, n)) { continue; }
          F32 e = elevation[(U64)n.y * w + n.x];
          // >= sea_level: the fill stops where water already is
          if(e >= params->sea_level && e <= basin_elevation + params->pond_epsilon) {
            elevation[(U64)n.y * w + n.x] = pond;
            bd_tile_at(board, n)->features[BD_Feature_River] = 0; // drowns any channel here
            pond_queue[pond_count] = n;
            pond_count += 1;
          }
        }
      }
    }
  }
  arena_release_scratch(scratch);
}

////////////////////////////////
//~ fp: Generation

internal BD_Board* wg_generate(Arena* arena, WG_Params* params, U64 seed) {
  BD_Board* board = bd_board_alloc(arena, params->width, params->height, 1024);

  //- fp: travel rules from the terrain table; the cost array shares the
  //  board's arena, so their lifetimes cannot drift apart
  F32* terrain_cost = push_array(arena, F32, params->terrain_count);
  for(U32 type = 0; type < params->terrain_count; type += 1) {
    terrain_cost[type] = params->terrains[type].move_cost;
  }
  board->rules.terrain_cost = terrain_cost;
  board->rules.terrain_cost_count = params->terrain_count;
  board->rules.road_cost = params->road_cost;
  board->rules.river_cross_cost = params->river_cross_cost;

  ArenaTemp scratch = arena_get_scratch(&arena, 1);
  I32 w = params->width;
  I32 h = params->height;

  //- fp: elevation field, kept for the whole generation -- classification
  //  and rivers must agree on where downhill is. Tectonic ridges come first:
  //  every later system (rivers, snowlines, foothills) hangs off elevation.
  V2* ridge_points = push_array_no_zero(scratch.arena, V2,
                                        (U64)Max(params->ridge_count, 1) * WG__RIDGE_SAMPLES);
  I32 ridge_point_count = wg__build_ridges(params, seed, ridge_points);
  F32* elevation = push_array_no_zero(scratch.arena, F32, (U64)w * h);
  for(I32 y = 0; y < h; y += 1) {
    for(I32 x = 0; x < w; x += 1) {
      elevation[(U64)y * w + x] =
          wg__elevation_at(params, seed, ridge_points, ridge_point_count, x, y);
    }
  }

  //- fp: rivers first: classification reads their presence, so river valleys
  //  come out wetter than the rain alone would make them
  wg__carve_rivers(board, params, seed, elevation);

  //- fp: classify tiles: prepare the fields, then first-match the terrain
  //  rows (see WG_TerrainDef). River banks read wetter than their moisture
  //  field says, so valleys go green or boggy.
  for(I32 y = 0; y < h; y += 1) {
    for(I32 x = 0; x < w; x += 1) {
      V2I p = {x, y};
      F32 e = elevation[(U64)y * w + x];
      F32 moisture = wg__moisture_at(params, seed, x, y);
      F32 drainage = wg__drainage_at(params, elevation, x, y);
      F32 temperature = wg__temperature_at(params, seed, e, x, y);

      B32 river_nearby = bd_feature_mask(board, p, BD_Feature_River) != 0;
      for(BD_Dir dir = 0; !river_nearby && dir < BD_Dir_COUNT; dir += 1) {
        river_nearby = bd_feature_mask(board, v2i_add(p, bd_dir_delta(dir)), BD_Feature_River) != 0;
      }
      if(river_nearby) { moisture = ClampTop(moisture + params->river_moisture, 1.0f); }

      B32 coast = 0;
      for(I32 dy = -1; !coast && dy <= 1; dy += 1) {
        for(I32 dx = -1; !coast && dx <= 1; dx += 1) {
          I32 nx = Clamp(0, x + dx, w - 1);
          I32 ny = Clamp(0, y + dy, h - 1);
          coast = elevation[(U64)ny * w + nx] < params->sea_level;
        }
      }

      bd_tile_at(board, p)->terrain =
          (BD_Terrain)wg__classify(params, e, moisture, drainage, temperature, coast);
    }
  }

  //- fp: despeckle: terrain regions smaller than min_region_size dissolve
  //  into their most common neighbor, repeated until the map is stable
  if(params->min_region_size > 1) {
    U8* visited = push_array_no_zero(scratch.arena, U8, (U64)w * h);
    V2I* queue = push_array_no_zero(scratch.arena, V2I, (U64)w * h);
    for(I32 pass = 0; pass < 8; pass += 1) {
      B32 merged = 0;
      MemoryZero(visited, (U64)w * h);
      for(I32 y = 0; y < h; y += 1) {
        for(I32 x = 0; x < w; x += 1) {
          if(visited[(U64)y * w + x]) { continue; }
          BD_Terrain terrain = bd_tile_at(board, (V2I){x, y})->terrain;
          visited[(U64)y * w + x] = 1;
          queue[0] = (V2I){x, y};
          I32 size = 1;
          for(I32 head = 0; head < size; head += 1) {
            for(BD_Dir dir = 0; dir < BD_Dir_COUNT; dir += 1) {
              V2I n = v2i_add(queue[head], bd_dir_delta(dir));
              if(!bd_in_bounds(board, n) || visited[(U64)n.y * w + n.x]) { continue; }
              if(bd_tile_at(board, n)->terrain != terrain) { continue; }
              visited[(U64)n.y * w + n.x] = 1;
              queue[size] = n;
              size += 1;
            }
          }
          if(size >= params->min_region_size) { continue; }
          U32 votes[WG_TERRAIN_CAP] = {0};
          for(I32 i = 0; i < size; i += 1) {
            for(BD_Dir dir = 0; dir < BD_Dir_COUNT; dir += 1) {
              V2I n = v2i_add(queue[i], bd_dir_delta(dir));
              if(!bd_in_bounds(board, n)) { continue; }
              BD_Terrain nt = bd_tile_at(board, n)->terrain;
              if(nt != terrain) { votes[nt] += 1; }
            }
          }
          U32 best = terrain;
          U32 best_votes = 0;
          for(U32 i = 0; i < params->terrain_count; i += 1) {
            if(votes[i] > best_votes) { best = i; best_votes = votes[i]; }
          }
          if(best_votes == 0) { continue; } // the whole map is one small region
          for(I32 i = 0; i < size; i += 1) {
            bd_tile_at(board, queue[i])->terrain = (BD_Terrain)best;
          }
          merged = 1;
        }
      }
      if(!merged) { break; }
    }
  }

  arena_release_scratch(scratch);
  return board;
}

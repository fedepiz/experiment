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
  params.continent_blend = tb_get_num(world, str8_lit("continent_blend"), 0);
  params.continent_height = tb_get_num(world, str8_lit("continent_height"), 0);
  params.min_region_size = (I32)tb_get_num(world, str8_lit("min_region_size"), 0);

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
  params.elevation_amplitude = Clamp(0.0f, params.elevation_amplitude, 1.0f);
  params.moisture_persistence = Clamp(0.05f, params.moisture_persistence, 1.0f);
  params.temperature_scale = ClampBot(params.temperature_scale, 1.0f);
  params.temperature_octaves = Clamp(1, params.temperature_octaves, 16);
  params.temperature_persistence = Clamp(0.05f, params.temperature_persistence, 1.0f);
  params.temperature_lapse_exponent = ClampBot(params.temperature_lapse_exponent, 0.1f);
  params.plate_spacing = ClampBot(params.plate_spacing, 2.0f);
  params.plate_fuzz = ClampBot(params.plate_fuzz, 0.0f);
  params.plate_fuzz_scale = ClampBot(params.plate_fuzz_scale, 1.0f);
  params.uplift_height = ClampBot(params.uplift_height, 0.0f);
  params.uplift_width = ClampBot(params.uplift_width, 0.5f);
  params.uplift_noise = Clamp(0.0f, params.uplift_noise, 1.0f);
  params.uplift_noise_scale = ClampBot(params.uplift_noise_scale, 1.0f);
  params.uplift_ridged = Clamp(0.0f, params.uplift_ridged, 1.0f);
  params.uplift_ridged_scale = ClampBot(params.uplift_ridged_scale, 1.0f);
  params.continent_blend = ClampBot(params.continent_blend, 1.0f);
  params.continent_height = Clamp(0.0f, params.continent_height, 1.0f);
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

// ridged fBm: each octave folds the signed noise around zero (1 - |2n - 1|,
// squared to sharpen the crease), so maxima land on the winding zero-crossing
// lines of smooth noise -- thin crests and V-valleys instead of soft blobs
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
// The map splits into tectonic plates: fuzzy voronoi cells around a jittered
// grid of seed points (one seed per plate_spacing-sized cell -- blue-noise-
// ish spacing, and density stays constant across map sizes). Each plate
// drifts with a hashed velocity; where the relative motion across a border
// presses the plates together, the border tiles seed a ridge whose strength
// is the compression. Uplift then falls off with distance to the nearest
// ridge tile (chamfer-propagated, near-euclidean), the distance warped by
// noise so ranges pinch and wander, and the result shaped by ridged noise so
// crests break into peaks and saddles instead of running as smooth walls.
// Plates also shape the sea: every plate touching the map border drowns into
// the continental rim, so coastlines follow plate borders (see wg__plate_fields).

#define WG__PLATE_SALT 0x6a09e667f3bcc909ull

internal V2 wg__plate_seed(WG_Params* params, U64 seed, I32 cx, I32 cy) {
  U64 salt = seed ^ WG__PLATE_SALT;
  V2 p;
  p.x = ((F32)cx + wg__noise01(salt + 1, cx, cy)) * params->plate_spacing;
  p.y = ((F32)cy + wg__noise01(salt + 2, cx, cy)) * params->plate_spacing;
  return p;
}

internal V2 wg__plate_velocity(U64 seed, I32 cx, I32 cy) {
  U64 salt = seed ^ WG__PLATE_SALT;
  F32 angle = 6.2831853f * wg__noise01(salt + 3, cx, cy);
  F32 magnitude = wg__noise01(salt + 4, cx, cy);
  return (V2){magnitude * cosf(angle), magnitude * sinf(angle)};
}

// fuzzy voronoi lookup: the query point warps by noise before the nearest-
// seed search, so borders wander organically; plate_fuzz = 0 degrades to
// exact voronoi. One seed per cell means scanning the two-ring around the
// warped point's cell finds the true nearest (worley-style).
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

// compression across the border between plates a and b: relative velocity
// projected on the a->b axis. Positive presses the plates together; unit-
// capped velocities bound it to [-2,2], so *0.5 normalizes into [0,1].
// Divergent and shearing borders score 0 -- rifts and trenches, later.
internal F32 wg__plate_compression(WG_Params* params, U64 seed, I32 gx, I32 a, I32 b) {
  V2 pa = wg__plate_seed(params, seed, a % gx, a / gx);
  V2 pb = wg__plate_seed(params, seed, b % gx, b / gx);
  V2 va = wg__plate_velocity(seed, a % gx, a / gx);
  V2 vb = wg__plate_velocity(seed, b % gx, b / gx);
  V2 axis = v2_norm(v2_sub(pb, pa), (V2){1, 0});
  F32 conv = (va.x - vb.x) * axis.x + (va.y - vb.y) * axis.y;
  return ClampBot(conv, 0.0f) * 0.5f;
}

// two-pass chamfer distance transform (1 / sqrt2 step costs), in place:
// dist starts at 0 on source cells and 1e9 elsewhere, and ends near-
// euclidean distance to the closest source. carry, if given, rides along,
// so each cell also ends holding its nearest source's payload.
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

// the whole plate pipeline: one uplift value per tile into out_uplift, and
// the continental rim factor (0 drowned .. 1 full ground) into out_rim
internal void wg__plate_fields(WG_Params* params, U64 seed, F32* out_uplift, F32* out_rim) {
  I32 w = params->width;
  I32 h = params->height;
  I32 gx = (I32)ceilf((F32)w / params->plate_spacing);
  I32 gy = (I32)ceilf((F32)h / params->plate_spacing);
  U64 salt = seed ^ WG__PLATE_SALT;
  ArenaTemp scratch = arena_get_scratch(0, 0);

  //- fp: assign every tile its plate
  I32* plate = push_array_no_zero(scratch.arena, I32, (U64)w * h);
  for(I32 y = 0; y < h; y += 1) {
    for(I32 x = 0; x < w; x += 1) {
      plate[(U64)y * w + x] = wg__plate_at(params, seed, gx, gy, x, y);
    }
  }

  //- fp: seed ridges: checking right and down covers every interior border
  //  edge exactly once; both sides join the ridge, and a tile touching two
  //  borders keeps the strongest press
  F32* dist = push_array_no_zero(scratch.arena, F32, (U64)w * h);
  F32* force = push_array_no_zero(scratch.arena, F32, (U64)w * h);
  for(U64 i = 0; i < (U64)w * h; i += 1) {
    dist[i] = 1e9f;
    force[i] = 0;
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
        F32 compression = wg__plate_compression(params, seed, gx, plate[i], plate[j]);
        if(compression <= 0) { continue; }
        dist[i] = 0;
        dist[j] = 0;
        force[i] = Max(force[i], compression);
        force[j] = Max(force[j], compression);
      }
    }
  }

  wg__distance_transform(dist, force, w, h);

  //- fp: continental rim, plate-shaped: any plate owning a border tile is
  //  ocean floor -- its whole cell reads rim 0 -- and land climbs back to
  //  full ground over continent_blend tiles inland of the drowned region,
  //  so the coastline follows the fuzzy plate borders, not the map
  //  rectangle. Every border tile belongs to some plate, so the map edge
  //  itself is always water.
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

  //- fp: uplift: a quadratic falloff of the noise-warped distance, scaled by
  //  the ridge's force, shaped by ridged noise
  for(I32 y = 0; y < h; y += 1) {
    for(I32 x = 0; x < w; x += 1) {
      U64 i = (U64)y * w + x;
      out_uplift[i] = 0;
      if(force[i] <= 0) { continue; }
      F32 d = dist[i];
      if(params->uplift_noise > 0) {
        F32 s = params->uplift_noise_scale;
        F32 wobble = wg__fbm(salt + 7, (F32)x / s, (F32)y / s, 2, 0.5f);
        d *= 1.0f + params->uplift_noise * (2.0f * wobble - 1.0f);
      }
      F32 t = Clamp(0.0f, d / params->uplift_width, 1.0f);
      F32 bell = (1.0f - t) * (1.0f - t);
      if(bell <= 0) { continue; }
      F32 shape = 1.0f;
      if(params->uplift_ridged > 0) {
        F32 s = params->uplift_ridged_scale;
        F32 ridged = wg__ridged_fbm(salt + 8, (F32)x / s, (F32)y / s, 3, 0.5f);
        shape += params->uplift_ridged * (ridged - 1.0f); // lerp(1 .. ridged)
      }
      out_uplift[i] = params->uplift_height * force[i] * bell * shape;
    }
  }
  arena_release_scratch(scratch);
}

////////////////////////////////
//~ fp: Fields
//
// Elevation is fBm plus tectonic uplift, drowned across the plate-shaped
// continental rim (see Tectonics), so the land reads as a continent with
// organic coastlines rather than wallpaper.

internal F32 wg__elevation_at(WG_Params* params, U64 seed, F32 uplift, F32 rim, I32 x, I32 y) {
  F32 noise = wg__fbm(seed, (F32)x / params->elevation_scale,
                      (F32)y / params->elevation_scale, params->elevation_octaves,
                      params->elevation_persistence);
  // three stacked terms: the continental shelf interior plates stand on,
  // noise spanning [0, elevation_amplitude] carving relief, and plate
  // uplift raising ranges -- all scaled by the rim, which sinks the whole
  // stack to the ocean floor across the drowned border plates. The clamp
  // keeps peaks inside the [0,1] domain the classification bands speak.
  F32 e = params->continent_height + noise * params->elevation_amplitude + uplift;
  return ClampTop(e, 1.0f) * rim;
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
  //  and rivers must agree on where downhill is. Tectonic uplift comes
  //  first: every later system (rivers, snowlines) hangs off elevation.
  F32* uplift = push_array_no_zero(scratch.arena, F32, (U64)w * h);
  F32* rim = push_array_no_zero(scratch.arena, F32, (U64)w * h);
  wg__plate_fields(params, seed, uplift, rim);
  F32* elevation = push_array_no_zero(scratch.arena, F32, (U64)w * h);
  for(I32 y = 0; y < h; y += 1) {
    for(I32 x = 0; x < w; x += 1) {
      U64 i = (U64)y * w + x;
      elevation[i] = wg__elevation_at(params, seed, uplift[i], rim[i], x, y);
    }
  }

  //- fp: rivers first: classification reads their presence, so river valleys
  //  come out wetter than the rain alone would make them
  wg__carve_rivers(board, params, seed, elevation);

  //- fp: the sea is the border-connected body of sub-sea_level water (the
  //  rim guarantees the border is ocean). Lakes and ponds are below sea
  //  level too but not border-connected, so their shores never read as
  //  coast -- beaches belong to the ocean.
  U8* sea = push_array(scratch.arena, U8, (U64)w * h);
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
      for(BD_Dir dir = 0; dir < BD_Dir_COUNT; dir += 1) {
        V2I n = v2i_add(queue[head], bd_dir_delta(dir));
        if(!bd_in_bounds(board, n) || sea[(U64)n.y * w + n.x]) { continue; }
        if(elevation[(U64)n.y * w + n.x] >= params->sea_level) { continue; }
        sea[(U64)n.y * w + n.x] = 1;
        queue[count] = n;
        count += 1;
      }
    }
  }

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
          coast = sea[(U64)ny * w + nx] != 0;
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

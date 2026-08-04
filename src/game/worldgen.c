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

internal WG_Params wg_params_load(String8 path) {
  ArenaTemp scratch = arena_get_scratch(0, 0);
  TB_Value* root = tb_parse_file_and_report(scratch.arena, path);
  TB_Value* world = tb_get(root, str8_lit("world"));

  WG_Params params = {0};
  params.width = (I32)tb_get_num(world, str8_lit("width"), 256);
  params.height = (I32)tb_get_num(world, str8_lit("height"), 256);

  params.elevation_scale = tb_get_num(world, str8_lit("elevation_scale"), 48);
  params.elevation_octaves = (I32)tb_get_num(world, str8_lit("elevation_octaves"), 4);
  params.moisture_scale = tb_get_num(world, str8_lit("moisture_scale"), 32);
  params.moisture_octaves = (I32)tb_get_num(world, str8_lit("moisture_octaves"), 3);

  params.sea_level = tb_get_num(world, str8_lit("sea_level"), 0.32f);
  params.mountain_level = tb_get_num(world, str8_lit("mountain_level"), 0.62f);
  params.forest_moisture = tb_get_num(world, str8_lit("forest_moisture"), 0.55f);
  params.continent_falloff = tb_get_num(world, str8_lit("continent_falloff"), 0.35f);

  params.river_count = (I32)tb_get_num(world, str8_lit("river_count"), 8);

  params.road_cost = tb_get_num(world, str8_lit("road_cost"), 0.5f);
  params.river_cross_cost = tb_get_num(world, str8_lit("river_cross_cost"), 2.0f);

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

internal F32 wg__fbm(U64 seed, F32 x, F32 y, I32 octaves) {
  F32 sum = 0;
  F32 total = 0;
  F32 amplitude = 1.0f;
  for(I32 octave = 0; octave < octaves; octave += 1) {
    sum += amplitude * wg__value_noise(seed + (U64)octave * 0x9E3779B97F4A7C15ull, x, y);
    total += amplitude;
    amplitude *= 0.5f;
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

internal F32 wg__elevation_at(WG_Params* params, U64 seed, I32 x, I32 y) {
  F32 e = wg__fbm(seed, (F32)x / params->elevation_scale,
                  (F32)y / params->elevation_scale, params->elevation_octaves);
  F32 nx = 2.0f * (F32)x / (F32)Max(params->width - 1, 1) - 1.0f; // [-1,1] across the map
  F32 ny = 2.0f * (F32)y / (F32)Max(params->height - 1, 1) - 1.0f;
  e -= params->continent_falloff * (nx * nx + ny * ny);
  return e;
}

// moisture decorrelates from elevation by salting the seed
#define WG__MOISTURE_SALT 0x8b3f9a1dcafeull

internal F32 wg__moisture_at(WG_Params* params, U64 seed, I32 x, I32 y) {
  return wg__fbm(seed ^ WG__MOISTURE_SALT, (F32)x / params->moisture_scale,
                 (F32)y / params->moisture_scale, params->moisture_octaves);
}

////////////////////////////////
//~ fp: Rivers
//
// Each river starts at the highest of a handful of hashed sample points and
// walks steepest-descent over the elevation field, connecting the river
// feature along the way, until it reaches water, flows off the map edge, or
// bottoms out in a basin (a spring that dies in a valley is credible enough).
// Rivers that cross an existing river simply merge -- connections are a mask,
// so re-connecting is idempotent.

#define WG__RIVER_SALT        0x517cc1b727220a95ull
#define WG__ELEVATION_OFF_MAP (-1000.0f)

internal void wg__carve_rivers(BD_Board* board, WG_Params* params, U64 seed, F32* elevation) {
  I32 w = board->width;
  I32 h = board->height;
  for(I32 river = 0; river < params->river_count; river += 1) {
    //- fp: source: highest of 32 hashed samples that isn't already underwater
    V2I source = {0};
    F32 source_elevation = WG__ELEVATION_OFF_MAP;
    for(I32 attempt = 0; attempt < 32; attempt += 1) {
      U64 salt = seed ^ WG__RIVER_SALT;
      V2I p = {(I32)(wg__hash(salt, river, attempt) % (U32)w),
               (I32)(wg__hash(salt + 1, river, attempt) % (U32)h)};
      F32 e = elevation[(U64)p.y * w + p.x];
      if(e > source_elevation && bd_tile_at(board, p)->terrain != WG_TerrainType_Water) {
        source = p;
        source_elevation = e;
      }
    }
    if(source_elevation <= WG__ELEVATION_OFF_MAP) { continue; } // all samples hit water

    //- fp: descend; step bound guards against float-tie pathologies
    V2I at = source;
    for(I32 step = 0; step < w + h; step += 1) {
      F32 here = elevation[(U64)at.y * w + at.x];
      BD_Dir down_dir = BD_Dir_COUNT;
      F32 down_elevation = here;
      B32 down_off_map = 0;
      for(BD_Dir dir = 0; dir < BD_Dir_COUNT; dir += 1) {
        V2I n = v2i_add(at, bd_dir_delta(dir));
        // off the map counts as the lowest place there is: coastal springs
        // near the border drain off the world instead of pooling
        F32 e = bd_in_bounds(board, n) ? elevation[(U64)n.y * w + n.x] : WG__ELEVATION_OFF_MAP;
        if(e < down_elevation) {
          down_dir = dir;
          down_elevation = e;
          down_off_map = !bd_in_bounds(board, n);
        }
      }
      if(down_dir == BD_Dir_COUNT) { break; } // basin: the river ends here

      bd_feature_connect(board, at, down_dir, BD_Feature_River);
      if(down_off_map) { break; }
      at = v2i_add(at, bd_dir_delta(down_dir));
      if(bd_tile_at(board, at)->terrain == WG_TerrainType_Water) { break; } // reached the sea
    }
  }
}

////////////////////////////////
//~ fp: Generation

internal BD_Board* wg_generate(Arena* arena, WG_Params* params, U64 seed) {
  BD_Board* board = bd_board_alloc(arena, params->width, params->height, 1024);

  //- fp: travel rules from the terrain table; the cost array shares the
  //  board's arena, so their lifetimes cannot drift apart
  F32* terrain_cost = push_array(arena, F32, WG_TerrainType_COUNT);
  for(WG_TerrainType type = 0; type < WG_TerrainType_COUNT; type += 1) {
    terrain_cost[type] = WG_TERRAIN_DATA[type].move_cost;
  }
  board->rules.terrain_cost = terrain_cost;
  board->rules.terrain_cost_count = WG_TerrainType_COUNT;
  board->rules.road_cost = params->road_cost;
  board->rules.river_cross_cost = params->river_cross_cost;

  ArenaTemp scratch = arena_get_scratch(&arena, 1);
  I32 w = params->width;
  I32 h = params->height;

  //- fp: elevation field, kept for the whole generation -- classification
  //  and rivers must agree on where downhill is
  F32* elevation = push_array_no_zero(scratch.arena, F32, (U64)w * h);
  for(I32 y = 0; y < h; y += 1) {
    for(I32 x = 0; x < w; x += 1) {
      elevation[(U64)y * w + x] = wg__elevation_at(params, seed, x, y);
    }
  }

  //- fp: classify tiles
  for(I32 y = 0; y < h; y += 1) {
    for(I32 x = 0; x < w; x += 1) {
      F32 e = elevation[(U64)y * w + x];
      WG_TerrainType type = WG_TerrainType_Plains;
      if(e < params->sea_level) {
        type = WG_TerrainType_Water;
      } else if(e > params->mountain_level) {
        type = WG_TerrainType_Mountain;
      } else if(wg__moisture_at(params, seed, x, y) > params->forest_moisture) {
        type = WG_TerrainType_Forest;
      }
      bd_tile_at(board, (V2I){x, y})->terrain = type;
    }
  }

  wg__carve_rivers(board, params, seed, elevation);

  arena_release_scratch(scratch);
  return board;
}

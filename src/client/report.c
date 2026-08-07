#include "base/core.h"
#include "base/math.h"
#include "base/arena.h"
#include "base/strings.h"
#include "base/print.h"
#include "base/tctx.h"
#include "game/thing_db.h"
#include "game/worldgen.h"
#include "client/report.h"

////////////////////////////////
//~ fp: temp: Worldgen Report

typedef struct {
  U16 group;
  U16 touches_border;
  I32 size;
} Report_Component;

// 4-connected components of equal group values; group 0 tiles are skipped
internal I32 report__components(Arena* arena, U16* groups, I32 w, I32 h,
                                Report_Component* out) {
  U8* visited = push_array(arena, U8, (U64)w * h);
  V2I* queue = push_array_no_zero(arena, V2I, (U64)w * h);
  I32 count = 0;
  for(I32 y = 0; y < h; y += 1) {
    for(I32 x = 0; x < w; x += 1) {
      U64 idx = (U64)y * w + x;
      if(visited[idx] || groups[idx] == 0) { continue; }
      Report_Component comp = {groups[idx], 0, 0};
      visited[idx] = 1;
      queue[0] = (V2I){x, y};
      I32 queue_count = 1;
      for(I32 head = 0; head < queue_count; head += 1) {
        V2I p = queue[head];
        comp.size += 1;
        if(p.x == 0 || p.y == 0 || p.x == w - 1 || p.y == h - 1) { comp.touches_border = 1; }
        for(Dir4 dir = 0; dir < Dir4_COUNT; dir += 1) {
          V2I n = v2i_add(p, dir4_delta(dir));
          if(n.x < 0 || n.y < 0 || n.x >= w || n.y >= h) { continue; }
          U64 nidx = (U64)n.y * w + n.x;
          if(visited[nidx] || groups[nidx] != comp.group) { continue; }
          visited[nidx] = 1;
          queue[queue_count] = n;
          queue_count += 1;
        }
      }
      out[count] = comp;
      count += 1;
    }
  }
  return count;
}

internal void report_worldgen(I32 world_count, U64 first_seed) {
  wg_terrain_table_load(str8_lit("data/world.tabula"));
  WG_Params params = wg_params_load(str8_lit("data/world.tabula"));

  // the report knows some terrains by role; ids resolve by name so the file
  // stays free to reorder
  U32 id_water = wg_terrain_by_name(str8_lit("water"));
  U32 id_ocean = wg_terrain_by_name(str8_lit("ocean"));
  U32 id_ice = wg_terrain_by_name(str8_lit("ice"));
  U32 id_beach = wg_terrain_by_name(str8_lit("beach"));
  U32 id_mountain = wg_terrain_by_name(str8_lit("mountain"));
  U32 id_snowcap = wg_terrain_by_name(str8_lit("snowcap"));
  B32 is_hot[WG_TERRAIN_CAP] = {0};
  B32 is_cold[WG_TERRAIN_CAP] = {0};
  is_hot[wg_terrain_by_name(str8_lit("desert"))] = 1;
  is_hot[wg_terrain_by_name(str8_lit("badlands"))] = 1;
  is_hot[wg_terrain_by_name(str8_lit("savanna"))] = 1;
  is_hot[wg_terrain_by_name(str8_lit("jungle"))] = 1;
  is_cold[id_ice] = 1;
  is_cold[id_snowcap] = 1;
  is_cold[wg_terrain_by_name(str8_lit("tundra"))] = 1;
  is_cold[wg_terrain_by_name(str8_lit("taiga"))] = 1;

  printf_str8("seed");
  for(U32 i = 1; i < params.terrain_count; i += 1) { printf_str8(",%S", wg_terrain_name(i)); }
  printf_str8(",land,landmasses,largest_landmass,ranges,largest_range,specks,lakes"
              ",river_tiles,river_nets,coast,beach_on_coast,hot_cold,nil,passable_largest\n");

  Arena* world_arena = arena_alloc();
  for(I32 world = 0; world < world_count; world += 1) {
    arena_clear(world_arena);
    U64 seed = first_seed + (U64)world;
    TH_Db* db = th_init_db(world_arena);
    wg_generate(db, &params, seed);
    I32 w = params.width;
    I32 h = params.height;
    U64 tiles = (U64)w * h;

    U32 counts[WG_TERRAIN_CAP] = {0};
    U16* by_terrain = push_array_no_zero(world_arena, U16, tiles); // terrain+1: no group 0
    U16* by_land = push_array(world_arena, U16, tiles);
    U16* by_range = push_array(world_arena, U16, tiles);
    U16* by_passable = push_array(world_arena, U16, tiles);
    U16* by_river = push_array(world_arena, U16, tiles);
    U64 river_tiles = 0;
    U64 coast = 0;
    U64 hot_cold = 0;
    for(I32 y = 0; y < h; y += 1) {
      for(I32 x = 0; x < w; x += 1) {
        U64 idx = (U64)y * w + x;
        V2I p = {x, y};
        U32 t = (U32)th_ifield_get(db, p, TH_IField_Terrain);
        counts[t] += 1;
        B32 land = t != 0 && t != id_water && t != id_ocean && t != id_ice;
        by_terrain[idx] = (U16)(t + 1);
        by_land[idx] = (U16)land;
        by_range[idx] = t == id_mountain || t == id_snowcap;
        by_passable[idx] = t < WG_TERRAIN_TYPE_COUNT && WG_TERRAIN_TYPES[t].move_cost > 0;
        by_river[idx] = th_ifield_get(db, p, TH_IField_RiverMask) != 0;
        river_tiles += by_river[idx];
        if(land) {
          B32 sea_beside = 0;
          for(Dir4 dir = 0; dir < Dir4_COUNT; dir += 1) {
            V2I n = v2i_add(p, dir4_delta(dir));
            U32 nt = (U32)th_ifield_get(db, n, TH_IField_Terrain);
            sea_beside |= th_world_in_bounds(db, n) && (nt == id_water || nt == id_ocean);
          }
          coast += sea_beside;
        }
        // unordered adjacent pairs: only look right and down
        for(U32 pair = 0; pair < 2; pair += 1) {
          V2I n = pair == 0 ? (V2I){x + 1, y} : (V2I){x, y + 1};
          if(!th_world_in_bounds(db, n)) { continue; }
          U32 nt = (U32)th_ifield_get(db, n, TH_IField_Terrain);
          hot_cold += (is_hot[t] && is_cold[nt]) || (is_cold[t] && is_hot[nt]);
        }
      }
    }

    Report_Component* comps = push_array_no_zero(world_arena, Report_Component, tiles);
    I32 speck_count = 0;
    I32 lake_count = 0;
    I32 comp_count = report__components(world_arena, by_terrain, w, h, comps);
    for(I32 i = 0; i < comp_count; i += 1) {
      U32 t = (U32)comps[i].group - 1;
      speck_count += comps[i].size <= 2;
      lake_count += (t == id_water || t == id_ocean) && !comps[i].touches_border;
    }
    I32 landmass_count = report__components(world_arena, by_land, w, h, comps);
    I32 largest_landmass = 0;
    U64 land_tiles = 0;
    for(I32 i = 0; i < landmass_count; i += 1) {
      largest_landmass = Max(largest_landmass, comps[i].size);
      land_tiles += (U64)comps[i].size;
    }
    I32 range_count = report__components(world_arena, by_range, w, h, comps);
    I32 largest_range = 0;
    for(I32 i = 0; i < range_count; i += 1) { largest_range = Max(largest_range, comps[i].size); }
    I32 passable_count = report__components(world_arena, by_passable, w, h, comps);
    U64 passable_tiles = 0;
    I32 largest_passable = 0;
    for(I32 i = 0; i < passable_count; i += 1) {
      largest_passable = Max(largest_passable, comps[i].size);
      passable_tiles += (U64)comps[i].size;
    }
    I32 river_net_count = report__components(world_arena, by_river, w, h, comps);

    printf_str8("%llu", (unsigned long long)seed);
    for(U32 i = 1; i < params.terrain_count; i += 1) {
      printf_str8(",%.4f", (F32)counts[i] / (F32)tiles);
    }
    printf_str8(",%.4f,%d,%.4f,%d,%d,%d,%d,%llu,%d,%llu,%.4f,%llu,%u,%.4f\n",
                (F32)land_tiles / (F32)tiles,
                landmass_count,
                land_tiles > 0 ? (F32)largest_landmass / (F32)land_tiles : 0.0f,
                range_count,
                largest_range,
                speck_count,
                lake_count,
                (unsigned long long)river_tiles,
                river_net_count,
                (unsigned long long)coast,
                coast > 0 ? (F32)counts[id_beach] / (F32)coast : 0.0f,
                (unsigned long long)hot_cold,
                counts[0],
                passable_tiles > 0 ? (F32)largest_passable / (F32)passable_tiles : 0.0f);
  }
}

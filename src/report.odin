package main

import "core:c/libc"
import "core:fmt"
import "core:mem/virtual"

import "game/thing"
import "game/worldgen"
import "geo"

////////////////////////////////
//~ fp: Worldgen Report
//
// `app worlds N [first_seed]` generates N worlds with no window, and writes
// one CSV row of measurements for each world to stdout. Read those rows with
// another program. The rows of a seed never change, so a diff of two runs
// tests that a change keeps the worlds.

// A printf rounds a decimal tie by the rule of its libc, and a fraction such
// as 800/25600 lands exactly on a tie. This function therefore prints the
// exact decimal expansion first, and rounds the STRING to four digits, with
// a tie away from zero. The report of a seed therefore reads the same on
// each toolchain. Every fraction of the report is 0 or more.
@(private)
csv_f4 :: proc(v: f32) -> string {
	// An f32 is a dyadic rational, so 24 digits after the point hold its exact
	// expansion for each value of this report.
	buf := make([]u8, 48, context.temp_allocator)
	n := libc.snprintf(raw_data(buf), 48, "%.24f", f64(v))
	s := buf[:n]
	dot := 0
	for s[dot] != '.' { dot += 1 }
	end := dot + 5 // keep four digits after the point
	if s[end] >= '5' {
		// round away from zero: add one at the last kept digit, with the carry
		for i := end - 1; i >= 0; i -= 1 {
			if s[i] == '.' { continue }
			if s[i] != '9' {
				s[i] += 1
				break
			}
			s[i] = '0'
			// A carry out of the first digit cannot occur here: each value is a
			// fraction below 10.
		}
	}
	return string(s[:end])
}

Report_Component :: struct {
	group: u16,
	touches_border: bool,
	size: int,
}

// The connected areas of equal group values, where two tiles connect across an
// edge and not across a corner. A tile of the group 0 does not join an area.
report_components :: proc(groups: []u16, w, h: int, out: []Report_Component,
                          allocator := context.allocator) -> int {
	visited := make([]u8, w * h, allocator)
	queue := make([]geo.V2i, w * h, allocator)
	count := 0
	for y in 0 ..< h {
		for x in 0 ..< w {
			idx := y * w + x
			if visited[idx] != 0 || groups[idx] == 0 { continue }
			comp := Report_Component{groups[idx], false, 0}
			visited[idx] = 1
			queue[0] = {x, y}
			queue_count := 1
			for head := 0; head < queue_count; head += 1 {
				p := queue[head]
				comp.size += 1
				if p.x == 0 || p.y == 0 || p.x == w - 1 || p.y == h - 1 { comp.touches_border = true }
				for dir in geo.Dir4 {
					n := p + geo.dir4_delta(dir)
					if n.x < 0 || n.y < 0 || n.x >= w || n.y >= h { continue }
					nidx := n.y * w + n.x
					if visited[nidx] != 0 || groups[nidx] != comp.group { continue }
					visited[nidx] = 1
					queue[queue_count] = n
					queue_count += 1
				}
			}
			out[count] = comp
			count += 1
		}
	}
	return count
}

report_worldgen :: proc(world_count: int, first_seed: u64) {
	worldgen.terrain_table_load("data/terrain_types.tabula")
	params := worldgen.params_load("data/world.tabula")

	// This report knows what some terrains do. It finds the id of such a
	// terrain by its name, so the order of the rows of the file can change.
	id_water := worldgen.terrain_by_name("water")
	id_ocean := worldgen.terrain_by_name("ocean")
	id_ice := worldgen.terrain_by_name("ice")
	id_beach := worldgen.terrain_by_name("beach")
	id_mountain := worldgen.terrain_by_name("mountain")
	id_snowcap := worldgen.terrain_by_name("snowcap")
	is_hot: [worldgen.TERRAIN_CAP]bool
	is_cold: [worldgen.TERRAIN_CAP]bool
	is_hot[worldgen.terrain_by_name("desert")] = true
	is_hot[worldgen.terrain_by_name("badlands")] = true
	is_hot[worldgen.terrain_by_name("savanna")] = true
	is_hot[worldgen.terrain_by_name("jungle")] = true
	is_cold[id_ice] = true
	is_cold[id_snowcap] = true
	is_cold[worldgen.terrain_by_name("tundra")] = true
	is_cold[worldgen.terrain_by_name("taiga")] = true

	fmt.printf("seed")
	for i in 1 ..< len(worldgen.TERRAIN_TYPES) { fmt.printf(",%s", worldgen.terrain_name(i)) }
	fmt.printf(",land,landmasses,largest_landmass,ranges,largest_range,specks,lakes" +
	           ",river_tiles,river_nets,coast,beach_on_coast,hot_cold,nil,passable_largest\n")

	world_arena: virtual.Arena
	if virtual.arena_init_growing(&world_arena) != nil { panic("out of memory: world arena") }
	world_allocator := virtual.arena_allocator(&world_arena)
	for world in 0 ..< world_count {
		free_all(world_allocator)
		free_all(context.temp_allocator)
		seed := first_seed + u64(world)
		db := thing.init(world_allocator)
		worldgen.generate(db, &params, seed)
		w := params.width
		h := params.height
		tiles := w * h

		counts: [worldgen.TERRAIN_CAP]int
		by_terrain := make([]u16, tiles, world_allocator) // the terrain plus 1, because the group 0 is not an area
		by_land := make([]u16, tiles, world_allocator)
		by_range := make([]u16, tiles, world_allocator)
		by_passable := make([]u16, tiles, world_allocator)
		by_river := make([]u16, tiles, world_allocator)
		river_tiles := 0
		coast := 0
		hot_cold := 0
		for y in 0 ..< h {
			for x in 0 ..< w {
				idx := y * w + x
				p := geo.V2i{x, y}
				t := int(thing.ifield_get(db, p, .Terrain))
				counts[t] += 1
				land := t != 0 && t != id_water && t != id_ocean && t != id_ice
				by_terrain[idx] = u16(t + 1)
				by_land[idx] = u16(land ? 1 : 0)
				by_range[idx] = u16(t == id_mountain || t == id_snowcap ? 1 : 0)
				by_passable[idx] = u16(t < len(worldgen.TERRAIN_TYPES) && worldgen.TERRAIN_TYPES[t].move_cost > 0 ? 1 : 0)
				by_river[idx] = u16(thing.ifield_get(db, p, .River_Mask) != 0 ? 1 : 0)
				river_tiles += int(by_river[idx])
				if land {
					sea_beside := false
					for dir in geo.Dir4 {
						n := p + geo.dir4_delta(dir)
						nt := int(thing.ifield_get(db, n, .Terrain))
						sea_beside |= thing.world_in_bounds(db, n) && (nt == id_water || nt == id_ocean)
					}
					coast += sea_beside ? 1 : 0
				}
				// Each pair of two tiles that touch, one time for each pair.
				// This loop therefore reads the tile to the right and the tile
				// below.
				for pair in 0 ..< 2 {
					n := pair == 0 ? geo.V2i{x + 1, y} : geo.V2i{x, y + 1}
					if !thing.world_in_bounds(db, n) { continue }
					nt := int(thing.ifield_get(db, n, .Terrain))
					hot_cold += (is_hot[t] && is_cold[nt]) || (is_cold[t] && is_hot[nt]) ? 1 : 0
				}
			}
		}

		comps := make([]Report_Component, tiles, world_allocator)
		speck_count := 0
		lake_count := 0
		comp_count := report_components(by_terrain, w, h, comps, world_allocator)
		for i in 0 ..< comp_count {
			t := int(comps[i].group) - 1
			speck_count += comps[i].size <= 2 ? 1 : 0
			lake_count += (t == id_water || t == id_ocean) && !comps[i].touches_border ? 1 : 0
		}
		landmass_count := report_components(by_land, w, h, comps, world_allocator)
		largest_landmass := 0
		land_tiles := 0
		for i in 0 ..< landmass_count {
			largest_landmass = max(largest_landmass, comps[i].size)
			land_tiles += comps[i].size
		}
		range_count := report_components(by_range, w, h, comps, world_allocator)
		largest_range := 0
		for i in 0 ..< range_count { largest_range = max(largest_range, comps[i].size) }
		passable_count := report_components(by_passable, w, h, comps, world_allocator)
		passable_tiles := 0
		largest_passable := 0
		for i in 0 ..< passable_count {
			largest_passable = max(largest_passable, comps[i].size)
			passable_tiles += comps[i].size
		}
		river_net_count := report_components(by_river, w, h, comps, world_allocator)

		fmt.printf("%d", seed)
		for i in 1 ..< len(worldgen.TERRAIN_TYPES) {
			fmt.printf(",%s", csv_f4(f32(counts[i]) / f32(tiles)))
		}
		fmt.printf(",%s,%d,%s,%d,%d,%d,%d,%d,%d,%d,%s,%d,%d,%s\n",
		           csv_f4(f32(land_tiles) / f32(tiles)),
		           landmass_count,
		           csv_f4(land_tiles > 0 ? f32(largest_landmass) / f32(land_tiles) : 0.0),
		           range_count,
		           largest_range,
		           speck_count,
		           lake_count,
		           river_tiles,
		           river_net_count,
		           coast,
		           csv_f4(coast > 0 ? f32(counts[id_beach]) / f32(coast) : 0.0),
		           hot_cold,
		           counts[0],
		           csv_f4(passable_tiles > 0 ? f32(largest_passable) / f32(passable_tiles) : 0.0))
	}
}

package worldgen

import "core:c/libc"
import "core:fmt"
import "core:math"
import "core:mem/virtual"
import "core:strings"

import "../../geo"
import "../../rng"
import "../../tabula"
import "../thing"

////////////////////////////////
//~ fp: Worldgen
//
// World generation makes a world from a set of parameters and a set of terrain
// rows. It reads both from a tabula file. It writes the world into the fields
// of the thing database: thing.I_Field.Terrain and the feature masks.
//
// A terrain id gets its meaning here. The id is the index of a row of the
// terrain table below.
//
// Generation is deterministic. The same parameters and the same seed give the
// same world. Each random value is a hash of integers, and the seed of
// generate is one of those integers. There is no state for a random number
// generator.
//
// The trigonometry and the power function go through libc, and not through
// core:math. A difference of one ulp in a noise value can move a tile across
// a band limit, which changes the world. A change of the math library
// therefore changes every world of every seed, so these calls must stay.

////////////////////////////////
//~ fp: Terrain Types
//
// The terrain table says what a terrain is, apart from the places where
// generation puts it. Each module reads the table. The index of a row is the
// terrain id everywhere: in a Terrain ifield value, in a tiling class, in a
// travel cost of the board, in an art prefix, and in a map color. Each row
// also holds the bands that claim it.
//
// terrain_table_load fills the whole table from the rows of the terrain
// file, in the order of the file. A second load therefore gives the same
// table. Call it before any code that reads the table, and before generate,
// which classifies each tile against the table.
//
// Row 0 is the nil terrain (ZII). A cell that generation does not paint reads
// as nil, which is impassable and bright magenta.
//
// The load clones each name onto an arena of the module, so the table needs
// no allocator from a caller. A load frees the names of the load before it,
// so a name stays valid until the next load.

TERRAIN_CAP :: 24

Terrain_Flag :: enum {
	Fertile,
}
Terrain_Flags :: bit_set[Terrain_Flag]

Band :: struct {
	min: f32,
	max: f32,
} // a band of 0 to 0 accepts each value

Terrain_Type :: struct {
	name: string,                         // the art prefix, and the name to show
	color: geo.V4,                        // the map color, for a terrain with no ground art
	rank: u8,                             // the order at a boundary. A higher rank covers a lower one.
	overlay_density: int,                 // the percent of the inner cells that carry an overlay
	move_cost: f32,                       // the cost to enter. A cost of 0 or less
	                                      // is impassable, as in board.Travel_Rules.
	flags: Terrain_Flags,                 // the traits of the terrain type

	//- fp: the field values that claim this row. See Terrain Classification below.
	elevation: Band,
	moisture: Band,
	drainage: Band,
	temperature: Band,
	needs_coast: bool, // the row claims a tile only when the sea is one step away
}

TERRAIN_TYPES: [dynamic;TERRAIN_CAP]Terrain_Type

////////////////////////////////
//~ fp: Terrain Classification
//
// The Band members above say which field values claim a terrain row.
//
// Classification takes the first row that matches. A tile takes the first row
// whose bands all contain the field values of that tile. A band is a closed
// interval. A band of 0 to 0 accepts each value (ZII), so a row with no band
// accepts each tile. Make the last row of the file such a row.
//
// The classifier starts at row 1. A tile that no row claims stays nil.
// terrain_table_load reports a gap of this kind in the band data.

@(private)
band_from_key :: proc(object: ^tabula.Value, key: string) -> Band {
	band: Band
	list := tabula.get(object, key)
	if list.kind == .List && list.first != nil {
		band.min = tabula.num_from_value(list.first)
		band.max = tabula.num_from_value(list.first.next)
	}
	return band
}

@(private)
band_contains :: proc(band: Band, v: f32) -> bool {
	if band.min == 0 && band.max == 0 { return true } // zero band = don't care
	return band.min <= v && v <= band.max
}

// The first row after the nil row whose bands all contain the field values.
// The result is 0 when no row claims the tile.
@(private)
classify :: proc(e, moisture, drainage, temperature: f32, coast: bool) -> int {
	for i in 1 ..< len(TERRAIN_TYPES) {
		def := &TERRAIN_TYPES[i]
		if !band_contains(def.elevation, e) { continue }
		if !band_contains(def.moisture, moisture) { continue }
		if !band_contains(def.drainage, drainage) { continue }
		if !band_contains(def.temperature, temperature) { continue }
		if def.needs_coast && !coast { continue }
		return i
	}
	return 0
}

// A set of bands is a box with sides that follow the axes. A gap in the
// coverage therefore contains the middle point of one cell of the grid that
// all the band limits make. A test of those middle points is an exact test.
// A gap writes a report to stderr, and each tile in it becomes the nil
// terrain, which is bright magenta.
@(private)
report_band_gaps :: proc(path: string) {
	cuts: [4][2 * TERRAIN_CAP + 2]f32
	cut_counts: [4]int
	for dim in 0 ..< 4 {
		cuts[dim][0] = 0
		cuts[dim][1] = 1
		cut_counts[dim] = 2
	}
	for i in 1 ..< len(TERRAIN_TYPES) {
		def := &TERRAIN_TYPES[i]
		bands := [4]Band{def.elevation, def.moisture, def.drainage, def.temperature}
		for dim in 0 ..< 4 {
			if bands[dim].min == 0 && bands[dim].max == 0 { continue }
			cuts[dim][cut_counts[dim]] = bands[dim].min
			cuts[dim][cut_counts[dim] + 1] = bands[dim].max
			cut_counts[dim] += 2
		}
	}
	gaps := 0
	for ei := 0; ei + 1 < cut_counts[0] && gaps < 4; ei += 1 {
		for mi := 0; mi + 1 < cut_counts[1] && gaps < 4; mi += 1 {
			for di := 0; di + 1 < cut_counts[2] && gaps < 4; di += 1 {
				for ti := 0; ti + 1 < cut_counts[3] && gaps < 4; ti += 1 {
					e := 0.5 * (cuts[0][ei] + cuts[0][ei + 1])
					m := 0.5 * (cuts[1][mi] + cuts[1][mi + 1])
					d := 0.5 * (cuts[2][di] + cuts[2][di + 1])
					t := 0.5 * (cuts[3][ti] + cuts[3][ti + 1])
					if classify(e, m, d, t, false) == 0 {
						fmt.eprintfln("%s: no terrain matches elevation %.2f moisture %.2f drainage %.2f temperature %.2f",
						              path, e, m, d, t)
						gaps += 1
					}
				}
			}
		}
	}
}

terrain_name :: proc(type: int) -> string { // empty for an index past the count
	if type < 0 || type >= len(TERRAIN_TYPES) { return "" }
	return TERRAIN_TYPES[type].name
}

terrain_type_get :: proc(idx: int) -> ^Terrain_Type {
	assert(idx < len(TERRAIN_TYPES))
	return &TERRAIN_TYPES[idx]
}

terrain_by_name :: proc(name: string) -> int { // 0, the nil terrain, for an unknown name
	for i in 0 ..< len(TERRAIN_TYPES) {
		if terrain_name(i) == name { return i }
	}
	return 0
}

@(private)
TERRAIN_FLAG_DEFS := [?]struct {
	name: string,
	flags: Terrain_Flags,
}{
	{"fertile", {.Fertile}},
}

// The flag of this name. A name that the table does not hold gives the empty
// set, which the caller must report: a wrong spelling must not read as "no
// flag".
terrain_flag_by_name :: proc(name: string) -> Terrain_Flags {
	result: Terrain_Flags
	for &def in TERRAIN_FLAG_DEFS {
		if def.name == name {
			result = def.flags
			break
		}
	}
	return result
}

// the memory of the names of the table. Each load frees it and fills it again.
@(private) terrain_name_arena: virtual.Arena

terrain_table_load :: proc(path: string) {
	root := tabula.parse_file_and_report(path, context.temp_allocator)
	name_allocator := virtual.arena_allocator(&terrain_name_arena)
	free_all(name_allocator)

	//- fp: the terrain rows, in the order of the file. Row 0 is the nil row,
	//  which this code writes. The load fills the whole table, so a second load
	//  gives the same table.
	TERRAIN_TYPES = {}
	append(&TERRAIN_TYPES, Terrain_Type{
		name = "nil",
		color = {1, 0, 1, 1}, // loud magenta
	})
	for node := root.first_member; node != nil; node = node.next {
		if node.key != "terrain_type" { continue }
		if len(TERRAIN_TYPES) >= TERRAIN_CAP {
			fmt.eprintfln("%s: more than %d terrain_type entries; extras ignored",
			              path, TERRAIN_CAP - 1)
			break
		}
		src := &node.value
		append(&TERRAIN_TYPES, Terrain_Type{})
		type := &TERRAIN_TYPES[len(TERRAIN_TYPES) - 1]

		// The parsed text is on the temp allocator, so the clone keeps the
		// name until the next load.
		name := strings.clone(tabula.get_string(src, "name") or_else "unnamed", name_allocator)
		type.name = name
		type.color = tabula.get_v4(src, "color") or_else {1, 0, 1, 1} // magenta = the loud fallback
		type.rank = u8(tabula.get_num(src, "rank"))
		type.overlay_density = int(tabula.get_num(src, "overlay_density"))
		type.move_cost = tabula.get_num(src, "move_cost") // missing = impassable, visibly

		type.elevation = band_from_key(src, "elevation")
		type.moisture = band_from_key(src, "moisture")
		type.drainage = band_from_key(src, "drainage")
		type.temperature = band_from_key(src, "temperature")
		type.needs_coast = tabula.get_num(src, "needs_coast") != 0

		//- fp: the flags, which are a list of names from TERRAIN_FLAG_DEFS. A
		//  row with no such list has no flag. A name that the table does not
		//  hold writes a report and adds nothing, so a wrong spelling cannot
		//  read as "this terrain does not have the trait".
		type.flags = {}
		flag_list := tabula.get(src, "flags")
		if !tabula.value_is_nil(flag_list) && flag_list.kind != .List {
			fmt.eprintfln("%s: terrain '%s': flags must be a list", path, name)
		}
		for elem := flag_list.first; elem != nil; elem = elem.next {
			flag_name := tabula.string_from_value(elem)
			flag := terrain_flag_by_name(flag_name)
			if flag == {} {
				fmt.eprintfln("%s: terrain '%s': unknown flag '%s'", path, name, flag_name)
			}
			type.flags |= flag
		}
	}
	report_band_gaps(path)
}

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

Params :: struct {
	width: int,
	height: int,

	//- fp: the noise fields. A scale is the tiles for each cell of the noise
	//  grid. A persistence is the decrease of the amplitude at each octave. A
	//  larger persistence gives a rougher field, with more variation inside the
	//  large shapes.
	elevation_scale: f32,
	elevation_octaves: int,
	elevation_persistence: f32,
	elevation_amplitude: f32, // the ceiling of the base noise, from 0 to 1. The
	                          // noise covers [0, amplitude], as the uplift covers
	                          // [0, uplift_height]. The generator adds the two.
	moisture_scale: f32,
	moisture_octaves: int,
	moisture_persistence: f32,

	//- fp: the temperature. It is a gradient from the north to the equator to
	//  the south, which is linear between those three points. The equator is
	//  the middle row of the map. The generator then adds a noise, and
	//  subtracts a fall in temperature for the land that stands above the sea.
	temperature_north: f32,
	temperature_equator: f32,
	temperature_south: f32,
	temperature_variation: f32,
	temperature_scale: f32,
	temperature_octaves: int,
	temperature_persistence: f32,
	temperature_lapse: f32,          // the fall at the highest elevation of the world
	temperature_lapse_exponent: f32, // the curve of that fall. A value above 1
	                                 // keeps the middle ground warm and makes
	                                 // the high ridges cold.

	//- fp: the meaning of the fields. The terrain rows hold the classification
	//  itself. These values fix the fields that the rows read. sea_level is the
	//  height of the water: a river runs down to it, and a coast test uses it.
	//  drainage_ceiling is the elevation where the term for the height above
	//  the water table reaches its largest value. river_moisture wets the banks
	//  of a river before the classification.
	sea_level: f32,
	drainage_ceiling: f32,
	drainage_full_slope: f32, // the slope, in elevation for each tile, that counts as fully drained
	river_moisture: f32,

	// A region of terrain that is smaller than this becomes the terrain of its
	// most common neighbour, after the classification. A value of 0 stops this
	// step. The band classification changes single tiles wherever a field is
	// near a limit, and a person reads those single tiles as noise and not as
	// terrain.
	min_region_size: int,

	//- fp: the tectonic plates. The map divides into voronoi cells with rough
	//  borders. Each cell grows around a seed point, and there is one seed
	//  point for each grid cell of the size plate_spacing. The size of a plate
	//  is therefore the same at any size of map.
	//
	//  Each plate moves with a velocity that a hash gives it. A border where
	//  two plates press together makes a ridge. The uplift decreases with the
	//  distance to the nearest ridge, and a noise moves that distance, so a
	//  range of mountains becomes narrow in places and turns. A ridged noise
	//  then shapes the uplift, so a crest breaks into peaks and passes and does
	//  not stay a smooth wall.
	plate_spacing: f32,       // the mean tiles between two plate seeds
	plate_fuzz: f32,          // the size of the border warp, in tiles. 0 gives an exact voronoi cell.
	plate_fuzz_scale: f32,    // the tiles for each cell of the warp noise
	uplift_height: f32,       // the elevation that a ridge at full force adds
	uplift_width: f32,        // the tiles from the line of a ridge to the foot of the fall
	uplift_noise: f32,        // the strength of the distance warp, from 0 to 1
	uplift_noise_scale: f32,  // the tiles for each cell of the distance warp noise
	uplift_ridged: f32,       // the part of the ridged noise in the shape, from 0 to 1
	uplift_ridged_scale: f32, // the tiles for each cell of the ridged noise

	//- fp: the borders where two plates move apart. The result depends on the
	//  crust. On land such a border sinks a rift valley, and the floor of that
	//  valley goes below the sea level in places and makes a long line of
	//  lakes. Under the sea such a border raises a ridge, which can reach the
	//  surface and make a line of volcanic islands. One field holds both, and
	//  the rim below mixes them.
	rift_depth: f32, // the elevation that a rift at full force removes on land
	rift_width: f32, // the tiles from the axis of a rift to the foot of the fall
	arc_height: f32, // the elevation that an ocean ridge at full force adds

	//- fp: the rim of a continent, which follows the plates. Each plate that
	//  owns a tile at the border of the map falls to an elevation of 0. The
	//  land then rises to the full height map over continent_blend tiles inland
	//  of that area. A coast therefore follows the rough border of a plate, and
	//  not the rectangle of the map.
	//
	//  continent_height is the floor that an inner plate stands on, which is
	//  the shelf above the deep sea. The structure of the plates therefore
	//  decides land against sea, and the noise and the uplift shape the relief
	//  above it.
	continent_blend: f32,
	continent_height: f32,

	river_count: int,      // the rivers to carve
	river_max_tries: int,  // the source points to try, to reach that count
	river_min_length: int, // the steps of the shortest river. The generator removes a shorter one.
	river_meander: f32,    // the change that the generator adds to each candidate
	                       // elevation as a river goes down, in elevation units.
	                       // A value of 0 gives the steepest path down.
	pond_epsilon: f32,     // A river can stop in a basin. The connected land
	                       // within this height of the floor of that basin then
	                       // fills with water.
	pond_max_tiles: int,   // the largest number of tiles of one pond, at any epsilon

	//- fp: the travel rule values that the game passes to the board
	road_cost: f32,
	river_cross_cost: f32,
}

params_load :: proc(path: string) -> Params {
	root := tabula.parse_file_and_report(path, context.temp_allocator)
	world := tabula.get(root, "world")

	// Each fallback is 0, and that is deliberate. A key that is absent or that
	// has a wrong spelling must give an obviously broken world, and not a
	// default that a reader cannot see. The file is the only source of these
	// values.
	params: Params
	params.width = int(tabula.get_num(world, "width"))
	params.height = int(tabula.get_num(world, "height"))

	params.elevation_scale = tabula.get_num(world, "elevation_scale")
	params.elevation_octaves = int(tabula.get_num(world, "elevation_octaves"))
	params.elevation_persistence = tabula.get_num(world, "elevation_persistence")
	params.elevation_amplitude = tabula.get_num(world, "elevation_amplitude")
	params.moisture_scale = tabula.get_num(world, "moisture_scale")
	params.moisture_octaves = int(tabula.get_num(world, "moisture_octaves"))
	params.moisture_persistence = tabula.get_num(world, "moisture_persistence")

	params.temperature_north = tabula.get_num(world, "temperature_north")
	params.temperature_equator = tabula.get_num(world, "temperature_equator")
	params.temperature_south = tabula.get_num(world, "temperature_south")
	params.temperature_variation = tabula.get_num(world, "temperature_variation")
	params.temperature_scale = tabula.get_num(world, "temperature_scale")
	params.temperature_octaves = int(tabula.get_num(world, "temperature_octaves"))
	params.temperature_persistence = tabula.get_num(world, "temperature_persistence")
	params.temperature_lapse = tabula.get_num(world, "temperature_lapse")
	params.temperature_lapse_exponent = tabula.get_num(world, "temperature_lapse_exponent")

	params.sea_level = tabula.get_num(world, "sea_level")
	params.drainage_ceiling = tabula.get_num(world, "drainage_ceiling")
	params.drainage_full_slope = tabula.get_num(world, "drainage_full_slope")
	params.river_moisture = tabula.get_num(world, "river_moisture")
	params.plate_spacing = tabula.get_num(world, "plate_spacing")
	params.plate_fuzz = tabula.get_num(world, "plate_fuzz")
	params.plate_fuzz_scale = tabula.get_num(world, "plate_fuzz_scale")
	params.uplift_height = tabula.get_num(world, "uplift_height")
	params.uplift_width = tabula.get_num(world, "uplift_width")
	params.uplift_noise = tabula.get_num(world, "uplift_noise")
	params.uplift_noise_scale = tabula.get_num(world, "uplift_noise_scale")
	params.uplift_ridged = tabula.get_num(world, "uplift_ridged")
	params.uplift_ridged_scale = tabula.get_num(world, "uplift_ridged_scale")
	params.rift_depth = tabula.get_num(world, "rift_depth")
	params.rift_width = tabula.get_num(world, "rift_width")
	params.arc_height = tabula.get_num(world, "arc_height")
	params.continent_blend = tabula.get_num(world, "continent_blend")
	params.continent_height = tabula.get_num(world, "continent_height")
	params.min_region_size = int(tabula.get_num(world, "min_region_size"))

	params.river_count = int(tabula.get_num(world, "river_count"))
	params.river_max_tries = int(tabula.get_num(world, "river_max_tries"))
	params.river_min_length = int(tabula.get_num(world, "river_min_length"))
	params.river_meander = tabula.get_num(world, "river_meander")
	params.pond_epsilon = tabula.get_num(world, "pond_epsilon")
	params.pond_max_tiles = int(tabula.get_num(world, "pond_max_tiles"))

	params.road_cost = tabula.get_num(world, "road_cost")
	params.river_cross_cost = tabula.get_num(world, "river_cross_cost")

	//- fp: the parameters whose value of 0 breaks the arithmetic, and does not
	//  only give a broken world. They are the divisors and the counts of
	//  octaves. An assert tests them, and no code changes them. A wrong value
	//  is a mistake in the file, and the person who wrote the file must correct
	//  it. Each other parameter gives a world that is visibly wrong on its own.
	assert(1 <= params.width && params.width <= thing.WORLD_MAX_DIM)
	assert(1 <= params.height && params.height <= thing.WORLD_MAX_DIM)
	assert(params.elevation_scale > 0 && params.elevation_octaves > 0)
	assert(params.moisture_scale > 0 && params.moisture_octaves > 0)
	assert(params.temperature_scale > 0 && params.temperature_octaves > 0)
	assert(params.plate_spacing > 0 && params.plate_fuzz_scale > 0)
	assert(params.uplift_noise_scale > 0 && params.uplift_ridged_scale > 0)
	assert(params.uplift_width > 0 && params.rift_width > 0)
	assert(params.continent_blend > 0)
	return params
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

@(private)
value_noise :: proc(seed: u64, x, y: f32) -> f32 {
	fx := math.floor(x)
	fy := math.floor(y)
	x0 := int(fx)
	y0 := int(fy)
	tx := x - fx
	ty := y - fy
	tx = tx * tx * (3.0 - 2.0 * tx) // smoothstep: kills the lattice creases
	ty = ty * ty * (3.0 - 2.0 * ty)
	n00 := rng.hash01_2d(seed, x0 + 0, y0 + 0)
	n10 := rng.hash01_2d(seed, x0 + 1, y0 + 0)
	n01 := rng.hash01_2d(seed, x0 + 0, y0 + 1)
	n11 := rng.hash01_2d(seed, x0 + 1, y0 + 1)
	nx0 := n00 + (n10 - n00) * tx
	nx1 := n01 + (n11 - n01) * tx
	return nx0 + (nx1 - nx0) * ty
}

@(private)
fbm :: proc(seed: u64, x, y: f32, octaves: int, persistence: f32) -> f32 {
	x, y := x, y
	sum: f32 = 0
	total: f32 = 0
	amplitude: f32 = 1.0
	for octave in 0 ..< octaves {
		sum += amplitude * value_noise(seed + u64(octave) * 0x9E3779B97F4A7C15, x, y)
		total += amplitude
		amplitude *= persistence
		x *= 2.0
		y *= 2.0
	}
	return sum / total
}

// A ridged fBm. Each octave folds its noise around zero with 1 - |2n - 1|, and
// squares the result to make the fold sharp. The largest values therefore lie
// on the lines where the smooth noise crosses zero. The field has thin crests
// and narrow valleys, and not soft round shapes.
@(private)
ridged_fbm :: proc(seed: u64, x, y: f32, octaves: int, persistence: f32) -> f32 {
	x, y := x, y
	sum: f32 = 0
	total: f32 = 0
	amplitude: f32 = 1.0
	for octave in 0 ..< octaves {
		n := value_noise(seed + u64(octave) * 0x9E3779B97F4A7C15, x, y)
		folded := 1.0 - abs(2.0 * n - 1.0)
		sum += amplitude * folded * folded
		total += amplitude
		amplitude *= persistence
		x *= 2.0
		y *= 2.0
	}
	return sum / total
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
// plates. See plate_fields.

@(private) PLATE_SALT :: 0x6a09e667f3bcc909

@(private)
plate_seed :: proc(params: ^Params, seed: u64, cx, cy: int) -> geo.V2 {
	salt := seed ~ PLATE_SALT
	p: geo.V2
	p.x = (f32(cx) + rng.hash01_2d(salt + 1, cx, cy)) * params.plate_spacing
	p.y = (f32(cy) + rng.hash01_2d(salt + 2, cx, cy)) * params.plate_spacing
	return p
}

@(private)
plate_velocity :: proc(seed: u64, cx, cy: int) -> geo.V2 {
	salt := seed ~ PLATE_SALT
	angle := 6.2831853 * rng.hash01_2d(salt + 3, cx, cy)
	magnitude := rng.hash01_2d(salt + 4, cx, cy)
	return {magnitude * libc.cosf(angle), magnitude * libc.sinf(angle)}
}

// The voronoi cell at a point, with a rough border. A noise moves the point
// before the search for the nearest seed, so a border turns. A plate_fuzz of 0
// gives an exact voronoi cell. There is one seed for each grid cell, so a scan
// of the two rings of cells around the moved point finds the true nearest
// seed.
@(private)
plate_at :: proc(params: ^Params, seed: u64, gx, gy: int, x, y: int) -> int {
	salt := seed ~ PLATE_SALT
	fx := f32(x)
	fy := f32(y)
	if params.plate_fuzz > 0 {
		s := params.plate_fuzz_scale
		fx += params.plate_fuzz * (2.0 * fbm(salt + 5, f32(x) / s, f32(y) / s, 2, 0.5) - 1.0)
		fy += params.plate_fuzz * (2.0 * fbm(salt + 6, f32(x) / s, f32(y) / s, 2, 0.5) - 1.0)
	}
	ccx := int(math.floor(fx / params.plate_spacing))
	ccy := int(math.floor(fy / params.plate_spacing))
	best := 0
	best_d2: f32 = 1e30
	for cy := max(ccy - 2, 0); cy <= min(ccy + 2, gy - 1); cy += 1 {
		for cx := max(ccx - 2, 0); cx <= min(ccx + 2, gx - 1); cx += 1 {
			p := plate_seed(params, seed, cx, cy)
			dx := fx - p.x
			dy := fy - p.y
			d2 := dx * dx + dy * dy
			if d2 < best_d2 {
				best_d2 = d2
				best = cy * gx + cx
			}
		}
	}
	return best
}

// The convergence across the border between plate a and plate b. It is the
// relative velocity along the axis from a to b, brought into [-1,1]. Each
// velocity has a length of 1 at most, so the dot product lies in [-2,2].
//
// A positive value presses the two plates together and makes a ridge. A
// negative value pulls them apart and makes a rift or an ocean ridge. Two
// plates that slide past each other give a value near 0, and no relief.
@(private)
plate_convergence :: proc(params: ^Params, seed: u64, gx: int, a, b: int) -> f32 {
	pa := plate_seed(params, seed, a % gx, a / gx)
	pb := plate_seed(params, seed, b % gx, b / gx)
	va := plate_velocity(seed, a % gx, a / gx)
	vb := plate_velocity(seed, b % gx, b / gx)
	axis := geo.norm(pb - pa, {1, 0})
	conv := (va.x - vb.x) * axis.x + (va.y - vb.y) * axis.y
	return conv * 0.5
}

// The shape of a slope, which a ridge and a rift both use. A noise moves the
// distance, so the feature becomes narrow in places and turns. The value then
// falls with the square of the distance over `width`. A ridged noise then
// gives the result its texture.
@(private)
flank_shape :: proc(params: ^Params, salt: u64, d: f32, width: f32, x, y: int) -> f32 {
	d := d
	if params.uplift_noise > 0 {
		s := params.uplift_noise_scale
		wobble := fbm(salt + 7, f32(x) / s, f32(y) / s, 2, 0.5)
		d *= 1.0 + params.uplift_noise * (2.0 * wobble - 1.0)
	}
	t := clamp(d / width, 0.0, 1.0)
	bell := (1.0 - t) * (1.0 - t)
	if bell <= 0 { return 0 }
	shape: f32 = 1.0
	if params.uplift_ridged > 0 {
		s := params.uplift_ridged_scale
		ridged := ridged_fbm(salt + 8, f32(x) / s, f32(y) / s, 3, 0.5)
		shape += params.uplift_ridged * (ridged - 1.0) // lerp(1 .. ridged)
	}
	return bell * shape
}

// A chamfer distance transform, in two passes, in place. A step to a side
// costs 1, and a step to a corner costs the square root of 2.
//
// At the start `dist` is 0 at each source cell and 1e9 at each other cell. At
// the end each cell holds a distance to the nearest source that is close to
// euclidean.
//
// `carry` can be nil. When it is present, each cell also ends with the value
// of its nearest source.
@(private)
distance_transform :: proc(dist: []f32, carry: []f32, w, h: int) {
	for y in 0 ..< h {
		for x in 0 ..< w {
			i := y * w + x
			ox := [4]int{-1, -1, 0, 1}
			oy := [4]int{0, -1, -1, -1}
			oc := [4]f32{1.0, 1.41421356, 1.0, 1.41421356}
			for k in 0 ..< 4 {
				nx := x + ox[k]
				ny := y + oy[k]
				if nx < 0 || nx >= w || ny < 0 || ny >= h { continue }
				j := ny * w + nx
				if dist[j] + oc[k] < dist[i] {
					dist[i] = dist[j] + oc[k]
					if carry != nil { carry[i] = carry[j] }
				}
			}
		}
	}
	for y := h - 1; y >= 0; y -= 1 {
		for x := w - 1; x >= 0; x -= 1 {
			i := y * w + x
			ox := [4]int{1, 1, 0, -1}
			oy := [4]int{0, 1, 1, 1}
			oc := [4]f32{1.0, 1.41421356, 1.0, 1.41421356}
			for k in 0 ..< 4 {
				nx := x + ox[k]
				ny := y + oy[k]
				if nx < 0 || nx >= w || ny < 0 || ny >= h { continue }
				j := ny * w + nx
				if dist[j] + oc[k] < dist[i] {
					dist[i] = dist[j] + oc[k]
					if carry != nil { carry[i] = carry[j] }
				}
			}
		}
	}
}

// The whole set of plate steps. It writes one uplift value at each tile into
// out_uplift. It writes the size of the divergence into out_rift, as a value
// from 0 to 1 that no parameter has scaled. It writes the rim of the continent
// into out_rim, where 0 is under the sea and 1 is full ground.
@(private)
plate_fields :: proc(params: ^Params, seed: u64, out_uplift, out_rift, out_rim: []f32) {
	w := params.width
	h := params.height
	gx := int(math.ceil(f32(w) / params.plate_spacing))
	gy := int(math.ceil(f32(h) / params.plate_spacing))
	salt := seed ~ PLATE_SALT
	scratch := context.temp_allocator

	//- fp: give each tile its plate
	plate := make([]int, w * h, scratch)
	for y in 0 ..< h {
		for x in 0 ..< w {
			plate[y * w + x] = plate_at(params, seed, gx, gy, x, y)
		}
	}

	//- fp: make the ridges and the rifts. A test to the right and a test
	//  downward reach each inner border edge one time. Both sides of a border
	//  join the feature. A tile that touches two borders keeps the strongest
	//  press and the strongest pull.
	dist := make([]f32, w * h, scratch)
	force := make([]f32, w * h, scratch)
	rdist := make([]f32, w * h, scratch)
	rforce := make([]f32, w * h, scratch)
	for i in 0 ..< w * h {
		dist[i] = 1e9
		force[i] = 0
		rdist[i] = 1e9
		rforce[i] = 0
	}
	for y in 0 ..< h {
		for x in 0 ..< w {
			i := y * w + x
			for k in 0 ..< 2 {
				nx := k == 0 ? x + 1 : x
				ny := k == 0 ? y : y + 1
				if nx >= w || ny >= h { continue }
				j := ny * w + nx
				if plate[i] == plate[j] { continue }
				conv := plate_convergence(params, seed, gx, plate[i], plate[j])
				if conv > 0 {
					dist[i] = 0
					dist[j] = 0
					force[i] = max(force[i], conv)
					force[j] = max(force[j], conv)
				}
				if conv < 0 {
					rdist[i] = 0
					rdist[j] = 0
					rforce[i] = max(rforce[i], -conv)
					rforce[j] = max(rforce[j], -conv)
				}
			}
		}
	}

	distance_transform(dist, force, w, h)
	distance_transform(rdist, rforce, w, h)

	//- fp: the rim of the continent, which follows the plates. A plate that
	//  owns a tile at the border of the map is a sea floor, and its whole cell
	//  reads a rim of 0. The land then rises to full ground over
	//  continent_blend tiles inland of that area. A coast therefore follows the
	//  rough borders of the plates, and not the rectangle of the map. Each tile
	//  at the border of the map belongs to some plate, so the edge of the map
	//  is always water.
	edge := make([]u8, gx * gy, scratch)
	for x in 0 ..< w {
		edge[plate[x]] = 1
		edge[plate[(h - 1) * w + x]] = 1
	}
	for y in 0 ..< h {
		edge[plate[y * w]] = 1
		edge[plate[y * w + (w - 1)]] = 1
	}
	rim_dist := make([]f32, w * h, scratch)
	for i in 0 ..< w * h {
		rim_dist[i] = edge[plate[i]] != 0 ? 0.0 : 1e9
	}
	distance_transform(rim_dist, nil, w, h)
	for i in 0 ..< w * h {
		t := min(rim_dist[i] / params.continent_blend, 1.0)
		out_rim[i] = t * t * (3.0 - 2.0 * t)
	}

	//- fp: the sizes of the uplift and of the rift. Each one is a shaped fall
	//  that the force scales. The uplift is the elevation to add. The rift
	//  stays a size from 0 to 1 that no parameter has scaled, because
	//  elevation_at gives it a meaning from the crust: rift_depth on land, and
	//  arc_height under the sea.
	for y in 0 ..< h {
		for x in 0 ..< w {
			i := y * w + x
			out_uplift[i] = 0
			out_rift[i] = 0
			if force[i] > 0 {
				m := flank_shape(params, salt, dist[i], params.uplift_width, x, y)
				out_uplift[i] = params.uplift_height * force[i] * m
			}
			if rforce[i] > 0 {
				m := flank_shape(params, salt, rdist[i], params.rift_width, x, y)
				out_rift[i] = rforce[i] * m
			}
		}
	}
}

////////////////////////////////
//~ fp: Fields
//
// The elevation is an fBm and a tectonic uplift. The rim of the continent,
// which follows the plates, then sinks that sum. See Tectonics. The land
// therefore reads as a continent with natural coasts, and not as a pattern
// that repeats.

@(private)
elevation_at :: proc(params: ^Params, seed: u64, uplift, rift, rim: f32, x, y: int) -> f32 {
	noise := fbm(seed, f32(x) / params.elevation_scale,
	             f32(y) / params.elevation_scale, params.elevation_octaves,
	             params.elevation_persistence)
	// Three terms, added together. The first is the shelf that an inner plate
	// stands on. The second is a noise across [0, elevation_amplitude], which
	// shapes the relief. The third is the plate uplift, which raises the
	// mountains. The rim then scales the sum, and sinks it to the sea floor
	// across the plates at the border of the map.
	e := params.continent_height + noise * params.elevation_amplitude + uplift
	e = min(e, 1.0) * rim
	// The divergence takes its meaning from the crust, and the rim mixes the
	// two results. On land it sinks a rift valley, and the floor of that valley
	// goes below the sea in places and makes a line of lakes. Under the sea it
	// raises a ridge, which can reach the surface as a line of islands.
	e += rift * (params.arc_height * (1.0 - rim) - params.rift_depth * rim)
	return clamp(e, 0.0, 1.0)
}

// The moisture uses a different seed, so it is independent of the elevation.
@(private) MOISTURE_SALT :: 0x8b3f9a1dcafe

@(private)
moisture_at :: proc(params: ^Params, seed: u64, x, y: int) -> f32 {
	return fbm(seed ~ MOISTURE_SALT, f32(x) / params.moisture_scale,
	           f32(y) / params.moisture_scale, params.moisture_octaves,
	           params.moisture_persistence)
}

@(private) TEMPERATURE_SALT :: 0x2545f4914f6cdd1d

// A gradient from the north to the equator to the south, which is linear
// between those three points. The equator is the middle row of the map. A
// noise then changes the value, and the height above the sea lowers it.
@(private)
temperature_at :: proc(params: ^Params, seed: u64, e: f32, x, y: int) -> f32 {
	lat := f32(y) / f32(max(params.height - 1, 1)) // 0 north edge, 1 south edge
	t := lat < 0.5 \
		? params.temperature_north + (params.temperature_equator - params.temperature_north) * (lat * 2.0) \
		: params.temperature_equator + (params.temperature_south - params.temperature_equator) * (lat * 2.0 - 1.0)
	wobble := fbm(seed ~ TEMPERATURE_SALT, f32(x) / params.temperature_scale,
	              f32(y) / params.temperature_scale, params.temperature_octaves,
	              params.temperature_persistence)
	t += params.temperature_variation * (wobble - 0.5)
	// The fall with the height follows a curve. The exponent keeps the middle
	// ground warm and makes the high ridges cold, so the snow stays on the
	// crests, and it does so at a warm latitude too.
	above := max(e - params.sea_level, 0.0) / max(1.0 - params.sea_level, 0.01)
	t -= params.temperature_lapse * libc.powf(above, params.temperature_lapse_exponent)
	return clamp(t, 0.0, 1.0)
}

// How easily water leaves a tile, from 0 to 1. This field is not a noise of
// its own. It comes from the landscape that the earlier steps built. Steep
// ground sheds water, and the slope term measures that from the differences of
// the elevation field. High ground stands above the water table, and the
// height term measures that. Low flat land near the sea level therefore reads
// near 0, which is where a swamp belongs.
@(private)
drainage_at :: proc(params: ^Params, elevation: []f32, x, y: int) -> f32 {
	w := params.width
	h := params.height
	xl := max(x - 1, 0); xr := min(x + 1, w - 1)
	yu := max(y - 1, 0); yd := min(y + 1, h - 1)
	dx := (elevation[y * w + xr] - elevation[y * w + xl]) / f32(max(xr - xl, 1))
	dy := (elevation[yd * w + x] - elevation[yu * w + x]) / f32(max(yd - yu, 1))
	slope := math.sqrt(dx * dx + dy * dy)
	slope_drain := min(slope / max(params.drainage_full_slope, 0.001), 1.0)
	e := elevation[y * w + x]
	height_drain := clamp((e - params.sea_level) / max(params.drainage_ceiling - params.sea_level, 0.01), 0.0, 1.0)
	return 0.6 * slope_drain + 0.4 * height_drain
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

@(private) RIVER_SALT :: 0x517cc1b727220a95
@(private) MEANDER_SALT :: 0x2545f4914f6cdd1d
@(private) ELEVATION_OFF_MAP :: f32(-1000.0)

// The two halves of a feature mask always agree. A connection from p toward d
// also sets the opposite bit at the neighbour of p. At the edge of the world
// the function writes the half at p only, so a river can flow off the world.
field_connect :: proc(db: ^thing.Db, p: geo.V2i, dir: geo.Dir4, mask_field: thing.I_Field) {
	thing.ifield_set_bit(db, p, mask_field, uint(dir), true)
	thing.ifield_set_bit(db, p + geo.dir4_delta(dir), mask_field, uint(geo.dir4_opposite(dir)), true)
}

@(private)
carve_rivers :: proc(db: ^thing.Db, params: ^Params, seed: u64, elevation: []f32) {
	w := params.width
	h := params.height
	// Each step goes to a lower tile, so a walk never reaches a tile two
	// times. w * h is therefore the exact limit of a walk.
	max_steps := w * h
	scratch := context.temp_allocator
	//- fp: the trace buffer. The code walks a river first, and writes its
	//  connections after. A river that stops before river_min_length steps
	//  therefore leaves no mark on the map.
	trace_pos := make([]geo.V2i, max_steps, scratch)
	trace_dir := make([]geo.Dir4, max_steps, scratch)
	pond_queue := make([]geo.V2i, params.pond_max_tiles, scratch)
	//- fp: the attempts, which chase a count. A river that is too short, and a
	//  source that the code cannot use, each cost one attempt and give no
	//  river. A map therefore reaches river_count, unless the terrain has no
	//  more room within river_max_tries.
	carved := 0
	for attempt := 0; attempt < params.river_max_tries && carved < params.river_count; attempt += 1 {
		//- fp: the source, which is the highest of 32 sample points on dry land
		//  that a hash chooses. The code rejects a sample that already carries
		//  a river. Each attempt therefore makes a new branch, and does not
		//  follow a channel that exists.
		source: geo.V2i
		source_elevation := ELEVATION_OFF_MAP
		for sample in 0 ..< 32 {
			salt := seed ~ RIVER_SALT
			p := geo.V2i{int(rng.hash_2d(salt, attempt, sample) % u32(w)),
			             int(rng.hash_2d(salt + 1, attempt, sample) % u32(h))}
			e := elevation[p.y * w + p.x]
			if e > source_elevation && e >= params.sea_level &&
			   thing.ifield_get(db, p, .River_Mask) == 0 {
				source = p
				source_elevation = e
			}
		}
		if source_elevation <= ELEVATION_OFF_MAP { continue } // no usable sample

		//- fp: walk down, and record each step. A candidate must be lower,
		//  which makes the walk stop. Among the candidates the code compares
		//  elevations that a hash changes a little, which makes the river turn.
		length := 0
		in_basin := false
		at := source
		for _ in 0 ..< max_steps {
			here := elevation[at.y * w + at.x]
			down_dir: geo.Dir4
			down_found := false
			down_score: f32 = 0
			down_off_map := false
			for dir in geo.Dir4 {
				n := at + geo.dir4_delta(dir)
				off_map := !thing.world_in_bounds(db, n)
				// A tile off the map is the lowest place. A spring near the
				// border therefore drains off the world, and does not make a
				// pond.
				e := off_map ? ELEVATION_OFF_MAP : elevation[n.y * w + n.x]
				if e >= here { continue } // only strictly downhill: no cycles
				score := e + params.river_meander *
				             (rng.hash01_2d(seed ~ MEANDER_SALT, n.x, n.y) - 0.5)
				if !down_found || score < down_score {
					down_dir = dir
					down_found = true
					down_score = score
					down_off_map = off_map
				}
			}
			if !down_found {
				in_basin = true
				break
			} // nowhere lower

			trace_pos[length] = at
			trace_dir[length] = down_dir
			length += 1
			if down_off_map { break }
			at = at + geo.dir4_delta(down_dir)
			if elevation[at.y * w + at.x] < params.sea_level { break } // reached the sea
		}

		if length < params.river_min_length { continue } // stubby spring: cull it
		carved += 1
		for idx in 0 ..< length {
			field_connect(db, trace_pos[idx], trace_dir[idx], .River_Mask)
		}

		//- fp: A river ends in the water. The half of the connection at the
		//  mouth tile would therefore draw a short piece inside the sea. Clear
		//  that half only, and keep the other half: the tile on the land then
		//  flows up to the edge of the water. A river that leaves the map ends
		//  on land, so the test below skips it.
		if !in_basin && elevation[at.y * w + at.x] < params.sea_level {
			thing.ifield_set(db, at, .River_Mask, 0)
		}

		//- fp: a river that stops inland fills its basin and makes a pond. The
		//  connected land within pond_epsilon of the height of that basin sinks
		//  to just below the sea level, and pond_max_tiles limits how much land
		//  sinks. The classification then paints a lake there, its shores take
		//  the wetter values of a river bank, and a later river can end in it
		//  as in a sea. Each tile that sinks goes below sea_level, so the fill
		//  can never reach it two times.
		if in_basin {
			basin_elevation := elevation[at.y * w + at.x]
			pond := params.sea_level - 0.01
			elevation[at.y * w + at.x] = pond
			thing.ifield_set(db, at, .River_Mask, 0) // no river inside the lake
			pond_queue[0] = at
			pond_count := 1
			for head := 0; head < pond_count; head += 1 {
				for dir in geo.Dir4 {
					n := pond_queue[head] + geo.dir4_delta(dir)
					if pond_count >= params.pond_max_tiles { break }
					if !thing.world_in_bounds(db, n) { continue }
					e := elevation[n.y * w + n.x]
					// The fill stops at a tile that holds water already.
					if e >= params.sea_level && e <= basin_elevation + params.pond_epsilon {
						elevation[n.y * w + n.x] = pond
						thing.ifield_set(db, n, .River_Mask, 0) // drowns any channel here
						pond_queue[pond_count] = n
						pond_count += 1
					}
				}
			}
		}
	}
}

@(private)
elevation_field :: proc(seed: u64, params: ^Params, allocator := context.allocator) -> []f32 {
	w := params.width
	h := params.height
	scratch := context.temp_allocator
	uplift := make([]f32, w * h, scratch)
	rift := make([]f32, w * h, scratch)
	rim := make([]f32, w * h, scratch)
	plate_fields(params, seed, uplift, rift, rim)
	elevation := make([]f32, w * h, allocator)
	for y in 0 ..< h {
		for x in 0 ..< w {
			i := y * w + x
			elevation[i] = elevation_at(params, seed, uplift[i], rift[i], rim[i], x, y)
		}
	}
	return elevation
}

@(private)
determine_sea :: proc(params: ^Params, elevation: []f32, allocator := context.allocator) -> []u8 {
	w := params.width
	h := params.height
	scratch := context.temp_allocator
	sea := make([]u8, w * h, allocator)
	{
		queue := make([]geo.V2i, w * h, scratch)
		count := 0
		for y in 0 ..< h {
			for x in 0 ..< w {
				if x != 0 && y != 0 && x != w - 1 && y != h - 1 { continue }
				if elevation[y * w + x] >= params.sea_level { continue }
				if sea[y * w + x] != 0 { continue }
				sea[y * w + x] = 1
				queue[count] = {x, y}
				count += 1
			}
		}
		for head := 0; head < count; head += 1 {
			for dir in geo.Dir4 {
				n := queue[head] + geo.dir4_delta(dir)
				if n.x < 0 || n.x >= w || n.y < 0 || n.y >= h || sea[n.y * w + n.x] != 0 { continue }
				if elevation[n.y * w + n.x] >= params.sea_level { continue }
				sea[n.y * w + n.x] = 1
				queue[count] = n
				count += 1
			}
		}
	}
	return sea
}

@(private)
classify_tiles :: proc(params: ^Params, seed: u64, db: ^thing.Db, elevation: []f32, sea: []u8) {
	//- fp: classify the tiles. Compute the fields, then take the first terrain
	//  row that matches. See Terrain_Type. A river bank reads wetter than its
	//  moisture field, so a valley becomes green or a swamp.
	w := params.width
	h := params.height
	for y in 0 ..< h {
		for x in 0 ..< w {
			p := geo.V2i{x, y}
			e := elevation[y * w + x]
			moisture := moisture_at(params, seed, x, y)
			drainage := drainage_at(params, elevation, x, y)
			temperature := temperature_at(params, seed, e, x, y)

			river_nearby := thing.ifield_get(db, p, .River_Mask) != 0
			for dir in geo.Dir4 {
				if river_nearby { break }
				river_nearby = thing.ifield_get(db, p + geo.dir4_delta(dir), .River_Mask) != 0
			}
			if river_nearby { moisture = min(moisture + params.river_moisture, 1.0) }

			coast := false
			for dy := -1; !coast && dy <= 1; dy += 1 {
				for dx := -1; !coast && dx <= 1; dx += 1 {
					nx := clamp(x + dx, 0, w - 1)
					ny := clamp(y + dy, 0, h - 1)
					coast = sea[ny * w + nx] != 0
				}
			}

			thing.ifield_set(db, p, .Terrain, i32(classify(e, moisture, drainage, temperature, coast)))
		}
	}
}

@(private)
despeckle :: proc(params: ^Params, db: ^thing.Db) {
	w := params.width
	h := params.height
	scratch := context.temp_allocator
	if params.min_region_size > 1 {
		visited := make([]u8, w * h, scratch)
		queue := make([]geo.V2i, w * h, scratch)
		for pass in 0 ..< 8 {
			merged := false
			for i in 0 ..< w * h { visited[i] = 0 }
			for y in 0 ..< h {
				for x in 0 ..< w {
					if visited[y * w + x] != 0 { continue }
					terrain := thing.ifield_get(db, {x, y}, .Terrain)
					visited[y * w + x] = 1
					queue[0] = {x, y}
					size := 1
					for head := 0; head < size; head += 1 {
						for dir in geo.Dir4 {
							n := queue[head] + geo.dir4_delta(dir)
							if !thing.world_in_bounds(db, n) || visited[n.y * w + n.x] != 0 { continue }
							if thing.ifield_get(db, n, .Terrain) != terrain { continue }
							visited[n.y * w + n.x] = 1
							queue[size] = n
							size += 1
						}
					}
					if size >= params.min_region_size { continue }
					votes: [TERRAIN_CAP]int
					for i in 0 ..< size {
						for dir in geo.Dir4 {
							n := queue[i] + geo.dir4_delta(dir)
							if !thing.world_in_bounds(db, n) { continue }
							nt := thing.ifield_get(db, n, .Terrain)
							if nt != terrain { votes[nt] += 1 }
						}
					}
					best := int(terrain)
					best_votes := 0
					for i in 0 ..< len(TERRAIN_TYPES) {
						if votes[i] > best_votes {
							best = i
							best_votes = votes[i]
						}
					}
					if best_votes == 0 { continue } // the whole map is one small region
					for i in 0 ..< size {
						thing.ifield_set(db, queue[i], .Terrain, i32(best))
					}
					merged = true
				}
			}
			if !merged { break }
		}
	}
}

////////////////////////////////
//~ fp: Generation
//
// generate writes a world into the thing database. It sets the size of the
// world, and an assert tests that size against thing.WORLD_MAX_DIM. It then
// carves each river down from the high ground, and classifies the terrain
// from the fields. It carves the rivers first, so the valley of a river reads
// as wetter land.
//
// Each result goes into a field of the database: Terrain and the feature
// masks. The generator writes over the values that those fields held.
//
// The parameters describe a family of worlds. `seed` chooses one member of
// that family.

generate :: proc(db: ^thing.Db, params: ^Params, seed: u64) {
	assert(params.width <= thing.WORLD_MAX_DIM && params.height <= thing.WORLD_MAX_DIM)
	thing.world_size_set(db, params.width, params.height)

	//- fp: generation owns each field of the world. It writes over the
	//  terrain, the rivers and the roads that the fields held, and keeps none
	//  of them.
	for y in 0 ..< params.height {
		for x in 0 ..< params.width {
			p := geo.V2i{x, y}
			thing.ifield_set(db, p, .Terrain, 0)
			thing.ifield_set(db, p, .River_Mask, 0)
			thing.ifield_set(db, p, .Road_Mask, 0)
		}
	}

	//- fp: the elevation field, which lives for the whole generation. The
	//  classification and the rivers must agree on the way down. The tectonic
	//  uplift comes first, because each later step reads the elevation: the
	//  rivers do, and the line of the snow does.
	elevation := elevation_field(seed, params, context.temp_allocator)

	//- fp: the rivers come first. The classification reads them, so a river
	//  valley becomes wetter than the rain alone would make it.
	carve_rivers(db, params, seed, elevation)

	//- fp: the sea is the water below sea_level that joins the border of the
	//  map. The rim makes each tile at that border a sea tile. A lake and a
	//  pond are also below sea_level, but they do not join the border, so their
	//  shores never read as a coast. A beach belongs to the sea only.
	sea := determine_sea(params, elevation, context.temp_allocator)

	//- fp: classify the tiles. Compute the fields, then take the first terrain
	//  row that matches. See Terrain_Type. A river bank reads wetter than its
	//  moisture field, so a valley becomes green or a swamp.
	classify_tiles(params, seed, db, elevation, sea)

	//- fp: remove the small regions. A region of terrain that is smaller than
	//  min_region_size becomes the terrain of its most common neighbour. The
	//  step repeats until the map stops to change.
	despeckle(params, db)
}

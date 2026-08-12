package game

import "core:fmt"
import "core:mem/virtual"
import "core:strings"

import "../chunk"
import "../color"
import "../geo"
import "../tabula"
import "board"
import "defs"
import "thing"
import "worldgen"

////////////////////////////////
//~ fp: Game
//
// The game layer is the simulated world, and nothing more. It is each part
// that a server with no display would hold.
//
// The thing database is the authority. It holds the things and the fields of
// the world, which are the terrain and the feature masks. The board comes from
// the database. The game keeps the board equal to the database, for the
// spatial queries and for pathfinding.
//
// Some things walk a loop of waypoints. Each one collects movement points on
// each tick, and pays the step cost of the board to move. A group therefore
// feels the speed of a terrain and does not only go around it: a step into a
// forest is slow, and a step along a road is fast.
//
// The caller supplies the time as `dt`. This layer reads no window, no input
// and no drawing.

// The database column that holds the connection mask of each board feature.
// The switch is exhaustive: a new defs.Feature member does not compile until
// its column exists, so the enum and the columns cannot drift apart.
feature_ifield :: proc(feature: defs.Feature) -> thing.I_Field {
	switch feature {
	case .River:
		return .River_Mask
	case .Road:
		return .Road_Mask
	}
	return .None
}

// the name of each feature to show, and its art prefix
FEATURE_NAMES :: defs.FEATURE_KEYS

////////////////////////////////
//~ fp: Sprites

Sprite :: enum {
	None,
	Band,
	Village,
	Palace,
	Herders,
	Wagon,
	Tholos,
}

// The name of each sprite to show, at the index of the enum. It is also the
// art prefix that the client reads, at assets/tiles/site_<key>_N.
SPRITE_NAMES :: [Sprite]string {
	.None    = "nil",
	.Band    = "band",
	.Village = "village",
	.Palace  = "palace",
	.Herders = "herders",
	.Wagon   = "wagon",
	.Tholos  = "tholos",
}

// Each quantity of the economy is whole, and the arithmetic that makes it is
// f32.
@(private)
floor_whole :: proc(value: f32) -> f32 {
	result := f32(i64(value))
	if result > value {result -= 1.0} 	// a cast of a value below 0 goes up
	return result
}

// For a ratio, which is the one number of the display that is not whole.
@(private)
display_ratio :: proc(value: f32) -> f32 {
	offset: f32 = value < 0 ? -0.5 : 0.5
	result := f32(i32(value * 100.0 + offset)) / 100.0
	if result > -0.005 && result < 0.005 {result = 0}
	return result
}

////////////////////////////////
//~ fp: Group Types
//
// The table is private to this package. No layer above the game reads a group
// type: the display gets the numbers of a group through info, as a tabula.
//
// Row 0 is the nil type (ZII). A group with row 0 draws nothing.
//
// Load after worldgen.terrain_table_load: a feeding rule names a terrain of
// it.

@(private)
GROUP_TYPE_CAP :: 16
@(private)
FEED_RULE_CAP :: 16

// A rule with no terrain and no flag matches each tile. Every rule that
// matches adds its yield, so the traits of a tile add together.
@(private)
Feed_Rule :: struct {
	terrain:   int, // the terrain id, or 0 for any terrain
	flags_all: worldgen.Terrain_Flags, // each flag that the terrain must hold
	yield:     f32, // the food that this rule adds, for one tile, in one tick
}

@(private)
Group_Type :: struct {
	name:           string, // the name to show, on the arena of the load
	sprite:         Sprite, // how a group of this type looks
	range:          f32, // the radius of the disc that the group draws from
	population_cap: f32, // the most people that can live here
	storage_share:  f32, // the part of each tick's food that goes to the granary first
	storage_cap:    f32, // the food that the granary holds
	rules:          [dynamic; FEED_RULE_CAP]Feed_Rule,
}

@(private)
GROUP_TYPES: [dynamic; GROUP_TYPE_CAP]Group_Type

// An index outside the table reads row 0, which is the nil type.
@(private)
group_type_get :: proc(idx: int) -> ^Group_Type {
	idx := idx
	if idx < 0 || idx >= len(GROUP_TYPES) {idx = 0}
	return &GROUP_TYPES[idx]
}

// Row 0 is the nil type, so the search starts at row 1 and an unknown name
// gives row 0.
@(private)
group_type_by_name :: proc(name: string) -> int {
	result := 0
	for i in 1 ..< len(GROUP_TYPES) {
		if GROUP_TYPES[i].name == name {
			result = i
			break
		}
	}
	return result
}

// The file and the group row that the loader reads now. It exists for the
// messages, so a load function does not carry two strings for its prefix. An
// empty `group` drops that part of the prefix.
@(private)
Group_Type_Source :: struct {
	path:  string,
	group: string,
}

@(private)
group_type_error :: proc(source: Group_Type_Source, format: string, args: ..any) {
	msg := fmt.tprintf(format, ..args)
	if len(source.group) > 0 {
		fmt.eprintfln("%s: group '%s': %s", source.path, source.group, msg)
	} else {
		fmt.eprintfln("%s: %s", source.path, msg)
	}
}

// The row of SPRITE_NAMES that holds `name`. Row 0 is the nil sprite, so the
// search starts at 1 and an unknown name gives 0.
@(private)
sprite_by_name :: proc(name: string) -> Sprite {
	names := SPRITE_NAMES
	for sprite in Sprite {
		if sprite == .None {continue}
		if names[sprite] == name {return sprite}
	}
	return .None
}

// One `feeds` entry of a type. A key that is absent leaves its part of the
// rule empty, and an empty part matches each tile.
@(private)
feed_rule_load :: proc(source: Group_Type_Source, type: ^Group_Type, entry: ^tabula.Value) {
	if len(type.rules) >= FEED_RULE_CAP {
		group_type_error(source, "more than %d feeds entries; extras ignored", FEED_RULE_CAP)
		return
	}
	rule: Feed_Rule
	rule.yield = tabula.get_num(entry, "yield")

	terrain_name, has_terrain := tabula.get_string(entry, "terrain")
	if has_terrain {
		rule.terrain = worldgen.terrain_by_name(terrain_name)
		// The nil terrain is row 0, and row 0 is also "any terrain" here. A
		// name that the terrain table does not hold therefore cannot become a
		// rule, because such a rule would add its yield on each tile of the
		// world.
		if rule.terrain == 0 {
			group_type_error(source, "unknown terrain '%s'; rule dropped", terrain_name)
			return
		}
	}

	flag_list := tabula.get(entry, "flags")
	if !tabula.value_is_nil(flag_list) && flag_list.kind != .List {
		group_type_error(source, "flags must be a list")
	}
	for elem := flag_list.first; elem != nil; elem = elem.next {
		flag_name := tabula.string_from_value(elem)
		flag := worldgen.terrain_flag_by_name(flag_name)
		if flag == {} {
			group_type_error(source, "unknown flag '%s'", flag_name)
		}
		rule.flags_all |= flag
	}

	append(&type.rules, rule)
}

// `allocator` holds the name of each type, so it must live as long as the
// table. init passes the arena of the game, which each new world clears.
@(private)
group_types_load :: proc(path: string, allocator := context.allocator) {
	root := tabula.parse_file_and_report(path, context.temp_allocator)
	source := Group_Type_Source{path, ""}

	GROUP_TYPES = {}
	append(&GROUP_TYPES, Group_Type{}) // row 0 stays the nil type

	for node := root.first_member; node != nil; node = node.next {
		if node.key != "group_type" {continue}
		if len(GROUP_TYPES) >= GROUP_TYPE_CAP {
			group_type_error(
				source,
				"more than %d group_type entries; extras ignored",
				GROUP_TYPE_CAP - 1,
			)
			break
		}
		body := &node.value
		append(&GROUP_TYPES, Group_Type{})
		type := &GROUP_TYPES[len(GROUP_TYPES) - 1]

		// The parsed text is on the temp allocator, which this frame releases.
		// The copy keeps the name for the life of the table.
		name := strings.clone(tabula.get_string(body, "name") or_else "unnamed", allocator)
		type.name = name
		source.group = name

		// The art of a group. A name that no sprite holds leaves the nil
		// sprite, which draws a flat shape.
		sprite_name, has_sprite := tabula.get_string(body, "sprite")
		type.sprite = sprite_by_name(sprite_name)
		if has_sprite && type.sprite == .None {
			group_type_error(source, "unknown sprite '%s'", sprite_name)
		}

		// Each fallback is 0, which gives a group with no reach and no people.
		// A key that is absent, and a key with a wrong spelling, therefore give
		// a group that is clearly broken on the screen.
		// A range is a distance and a share is a fraction, so both keep their
		// decimals. A count of people and an amount of food are quantities, so
		// both become whole numbers.
		type.range = tabula.get_num(body, "range")
		type.storage_share = clamp(tabula.get_num(body, "storage_share"), 0.0, 1.0)
		type.population_cap = floor_whole(tabula.get_num(body, "population_cap"))
		type.storage_cap = floor_whole(tabula.get_num(body, "storage_cap"))

		for member := body.first_member; member != nil; member = member.next {
			if member.key != "feeds" {continue}
			feed_rule_load(source, type, &member.value)
		}
		source.group = ""
	}
}

// The type of a group. A group with no type reads row 0, which takes nothing.
@(private)
group_type_of :: proc(db: ^thing.Db, id: thing.Id) -> ^Group_Type {
	return group_type_get(int(thing.ivar_get(db, id, .Group_Type)))
}

// Before the division between the groups that reach that tile.
@(private)
tile_yield :: proc(db: ^thing.Db, type: ^Group_Type, pos: geo.V2i) -> f32 {
	terrain := int(thing.ifield_get(db, pos, .Terrain))
	if terrain >= len(worldgen.TERRAIN_TYPES) {return 0.0}
	flags := worldgen.TERRAIN_TYPES[terrain].flags
	yield: f32 = 0.0
	for &rule in type.rules {
		if rule.terrain != 0 && rule.terrain != terrain {continue}
		if rule.flags_all & flags != rule.flags_all {continue}
		yield += rule.yield
	}
	return yield
}

////////////////////////////////
//~ fp: Selection
//
// The selection is what the player has chosen: nothing, a tile, or a thing. A
// selection of a thing holds the id of that thing. The game computes its tile
// again at each update, so the selection follows the thing, and the game
// clears the selection when the thing stops to exist.

Selection_Kind :: enum {
	None, // the player chose nothing (ZII)
	Tile,
	Thing,
}

Selection :: struct {
	kind: Selection_Kind,
	tile: geo.V2i, // Tile: the cell that the player chose. Thing: the tile of
	// the thing, as of the last update.
	id:   thing.Id, // Thing only
}

Game :: struct {
	arena:       virtual.Arena, // the memory of the game. init makes it at the first
	// call, and clears it for each new world.
	arena_ready: bool,
	initialised: bool,
	paused:      bool,
	tick_num:    u64,
	db:          ^thing.Db,
	board:       ^board.Board,
	move_timer:  f32,
	selection:   Selection,
}

////////////////////////////////
//~ fp: Board Mirror

// Copy the fields of the database into the board: the terrain ids, the
// feature masks, and the travel rules, whose costs come from the terrain
// table. Then drop the path cache. Call this after each write to a field.
#assert(worldgen.TERRAIN_CAP <= board.TERRAIN_CAP)
@(private)
board_mirror :: proc(game: ^Game, params: ^worldgen.Params) {
	db := game.db
	b := game.board
	for y in 0 ..< b.height {
		for x in 0 ..< b.width {
			p := geo.V2i{x, y}
			tile := board.tile_at(b, p)
			tile.terrain = board.Terrain(thing.ifield_get(db, p, .Terrain))
			for feature in defs.Feature {
				tile.features[feature] = u8(thing.ifield_get(db, p, feature_ifield(feature)))
			}
		}
	}
	rules := &b.rules
	rules.terrain_cost = {}
	for &type in worldgen.TERRAIN_TYPES {
		append(&rules.terrain_cost, type.move_cost)
	}
	rules.road_cost = params.road_cost
	rules.river_cross_cost = params.river_cross_cost
	board.path_cache_clear(b)
}

// A road for a demonstration. It joins two points on opposite sides of the
// continent, and it follows the best path across the terrain. The function
// finds the path on the board, writes the road into the fields, then copies
// the fields into the board again.
@(private)
demo_road :: proc(game: ^Game, params: ^worldgen.Params) {
	b := game.board
	west := board.snap_passable(b, {b.width / 6, b.height / 2})
	east := board.snap_passable(b, {b.width * 5 / 6, b.height / 2})
	path := board.path_find(b, west, east, context.temp_allocator)
	for idx in 0 ..< len(path.points) - 1 {
		dir, ok := geo.dir4_from_delta(path.points[idx + 1] - path.points[idx])
		if ok {worldgen.field_connect(game.db, path.points[idx], dir, .Road_Mask)}
	}
	board_mirror(game, params)
}

@(private)
xy_get :: proc(db: ^thing.Db, id: thing.Id) -> geo.V2i {
	return {int(thing.ivar_get(db, id, .X)), int(thing.ivar_get(db, id, .Y))}
}

@(private)
xy_set :: proc(db: ^thing.Db, id: thing.Id, pos: geo.V2i) {
	thing.ivar_set(db, id, .X, i32(pos.x))
	thing.ivar_set(db, id, .Y, i32(pos.y))
}

////////////////////////////////
//~ fp: Tile Queries
//
// A tile query finds the tiles near a point that pass a set of filters. Use it
// to choose a site: where a group can settle, and where it can go.

@(private)
Tile_Hit :: struct {
	coords: geo.V2i,
}

@(private)
Query_Tiles_Flag :: enum {
	Unclaimed,
}
@(private)
Query_Tiles_Flags :: bit_set[Query_Tiles_Flag]

@(private)
Query_Tiles :: struct {
	focus:             geo.V2i,
	radius_min:        int,
	radius_max:        int,
	terrain_flags_any: worldgen.Terrain_Flags,
	terrain_flags_all: worldgen.Terrain_Flags,
	query_flags_all:   Query_Tiles_Flags,
}

@(private)
query_tiles_make :: proc(focus: geo.V2i, range: int) -> Query_Tiles {
	query: Query_Tiles
	query.focus = focus
	query.radius_max = range
	return query
}

@(private)
query_tiles_run :: proc(
	game: ^Game,
	query: Query_Tiles,
	allocator := context.allocator,
) -> []Tile_Hit {
	b := game.board
	db := game.db

	// The walk computes the extents, the distance test and the empty cases, so
	// this function holds the filters of the query and nothing more.
	disc := board.disc_ring(b, query.focus, f32(query.radius_min), f32(query.radius_max))
	hits_cap := board.disc_bound(disc)
	if hits_cap == 0 {return nil}
	tiles := make([]Tile_Hit, hits_cap, allocator)
	count := 0

	for board.disc_next(&disc) {
		pos := disc.pos
		tile := board.tile_at(b, pos)
		terrain_type := worldgen.terrain_type_get(int(tile.terrain))

		if query.terrain_flags_any != {} && terrain_type.flags & query.terrain_flags_any == {} {
			continue
		}

		if terrain_type.flags & query.terrain_flags_all != query.terrain_flags_all {
			continue
		}

		if .Unclaimed in query.query_flags_all {
			if thing.field_ref_get(db, .Home, pos) != 0 {continue}
		}

		assert(count < hits_cap)
		tiles[count] = {pos}
		count += 1
	}
	return tiles[:count]
}

////////////////////////////////
//~ fp: Extraction

// Extraction writes the facts of the database into the layers that come from
// it. It writes them in full, on each tick. Today the board is the only such
// layer. A later index, roster or set of display data goes into the same
// single pass over the things, and reads the facts that it needs. No other
// code writes those layers.
@(private)
extract :: proc(game: ^Game) {
	db := game.db

	//- fp: the board, from the side of the pawns first. The sweep examines
	//  every pawn, so it removes each pawn whose thing is gone or is no longer
	//  on the board.
	pawns := board.pawns_all(game.board, context.temp_allocator)
	for pawn in pawns {
		// An old id reads no flag, so a thing that is gone fails this test too.
		id := thing.Id(pawn.key)
		if !thing.flag_get(db, id, .Placed) {
			board.pawn_remove(game.board, u64(id))
		}
	}

	//- fp: the side of the things, which is the one loop that each extraction
	//  shares. A thing on the board writes the position of its pawn here, and
	//  makes that pawn when it does not exist. Only this loop can make a pawn,
	//  because a sweep of the pawns never reaches a pawn that is absent.
	for this := thing.first(db); this != 0; this = thing.next(db, this) {
		if thing.flag_get(db, this, .Placed) {
			board.pawn_place(game.board, u64(this), xy_get(db, this))
		}
	}
}

////////////////////////////////
//~ fp: Init

// A caller must call worldgen.terrain_table_load first: the generator and the
// feeding rules read the terrain table.
init :: proc(game: ^Game, seed: u64) {
	// The arena survives the zeroing below: the game owns it for the run of
	// the program, and each new world clears it.
	if !game.arena_ready {
		if virtual.arena_init_growing(&game.arena) != nil {panic("out of memory: game arena")}
	}
	allocator := virtual.arena_allocator(&game.arena)
	free_all(allocator)
	arena := game.arena
	game^ = {}
	game.arena = arena
	game.arena_ready = true

	game.db = thing.init(allocator)

	//- fp: the world. Read the parameters from the files, generate the world
	//  into the fields, then copy the fields into the board. The caller has
	//  loaded the terrain table already: a feeding rule names a terrain and a
	//  flag of it.
	group_types_load("data/group_types.tabula", allocator)
	params := worldgen.params_load("data/world.tabula")
	worldgen.generate(game.db, &params, seed)
	game.board = board.alloc(allocator, params.width, params.height, 1024)
	board_mirror(game, &params)
	demo_road(game, &params)

	db := game.db

	// Waypoints near the corners. Each one moves to the nearest land that this
	// world grew there. The Next ref joins them into a loop.
	corners := [?]geo.V2i{{30, 30}, {220, 40}, {210, 210}, {40, 220}}
	waypoints: [len(corners)]thing.Id
	for i in 0 ..< len(corners) {
		id := thing.spawn(db)
		xy_set(db, id, board.snap_passable(game.board, corners[i]))
		waypoints[i] = id
	}

	for i in 0 ..< len(waypoints) {
		thing.ref_set(db, .Next, waypoints[i], waypoints[(i + 1) % len(waypoints)])
	}

	for i in 0 ..< 3 {
		id := thing.spawn(db)
		xy_set(db, id, xy_get(db, waypoints[i]))
		thing.ref_set(db, .Goal, id, waypoints[(i + 1) % len(waypoints)])
		thing.ivar_set(db, id, .Sprite, i32(Sprite.Wagon))
		thing.flag_set(db, id, .Placed, true)
		thing.flag_set(db, id, .Mobile, true)
	}

	Group_Spawn :: struct {
		name:       string,
		pos:        geo.V2i,
		type:       string,
		population: f32,
	}

	// The discs of the first two cross across many tiles, so each of those
	// tiles gives each group one half. The third has a wide reach and crosses
	// both, so two kinds of way of life draw from one place. The fourth is far
	// from the other three, and shows what land gives to one group alone.
	GROUP_SPAWNS :: [?]Group_Spawn {
		{"Ashfield", {102, 96}, "village", 10},
		{"Two Rivers", {106, 97}, "village", 10},
		{"Longgrass", {96, 104}, "herders", 10},
		{"Redhill", {88, 118}, "band", 10},
	}

	is_first := true
	for spawn in GROUP_SPAWNS {
		// An unknown name gives row 0: a group with no reach and no people,
		// which is clearly broken on the screen.
		type_idx := group_type_by_name(spawn.type)
		if type_idx == 0 {
			fmt.eprintfln("spawn '%s': unknown group type '%s'", spawn.name, spawn.type)
		}
		type := group_type_get(type_idx)

		id := thing.spawn(db)
		name := thing.label(db, id, .Name)
		thing.push_word(name, thing.define_word(db, spawn.name))
		xy_set(db, id, board.snap_passable(game.board, spawn.pos))
		thing.ivar_set(db, id, .Group_Type, i32(type_idx))
		thing.ivar_set(db, id, .Sprite, i32(type.sprite))
		thing.var_set(db, id, .Population, floor_whole(spawn.population))
		thing.flag_set(db, id, .Has_Influence, true)
		thing.flag_set(db, id, .Placed, true)
		thing.flag_set(db, id, .Player, is_first)
		is_first = false
	}

	thing.commit(db)
	extract(game)
	game.initialised = true
}

////////////////////////////////
//~ fp: Selection

// A thing that stands on the tile wins against the tile. A tile off the board
// clears the selection.
select :: proc(game: ^Game, tile: geo.V2i) {
	selection: Selection
	if board.in_bounds(game.board, tile) {
		selection.kind = .Tile
		selection.tile = tile
		pawn := board.tile_at(game.board, tile).first_pawn
		if pawn != nil {
			selection.kind = .Thing
			selection.id = thing.Id(pawn.key)
		}
	}
	game.selection = selection
}

deselect :: proc(game: ^Game) {
	game.selection = {}
}

// Read the tile of a selected thing from that thing again. A thing that is
// gone, and a thing that is no longer on the board, clear the selection. This
// runs at each update.
@(private)
selection_refresh :: proc(game: ^Game) {
	if game.selection.kind == .Thing {
		if thing.flag_get(game.db, game.selection.id, .Placed) {
			game.selection.tile = xy_get(game.db, game.selection.id)
		} else {
			game.selection = {}
		}
	}
}

@(private)
thing_name :: proc(db: ^thing.Db, id: thing.Id, allocator := context.allocator) -> string {
	return thing.resolve_phrase(db, thing.label(db, id, .Name)^, "", allocator)
}

////////////////////////////////
//~ fp: Info
//
// the facts of the game, as a tabula

// The forms of `type` are in the typed rows of the hud.
@(private)
fact_row :: proc(
	out: ^tabula.Value,
	key, name, type: string,
	allocator := context.allocator,
) -> ^tabula.Value {
	row := tabula.add_object(out, key, allocator)
	tabula.add_string(row, "name", name, allocator)
	tabula.add_string(row, "type", type, allocator)
	return row
}

@(private)
tile_facts :: proc(game: ^Game, out: ^tabula.Value, pos: geo.V2i, allocator := context.allocator) {
	db := game.db
	xy := fact_row(out, "pos", "Position", "text", allocator)
	tabula.add_string(
		xy,
		"value",
		fmt.aprintf("%d, %d", pos.x, pos.y, allocator = allocator),
		allocator,
	)

	terrain := int(thing.ifield_get(db, pos, .Terrain))
	terrain_row := fact_row(out, "terrain", "Terrain", "text", allocator)
	tabula.add_string(terrain_row, "value", worldgen.terrain_name(terrain), allocator)
	if terrain < len(worldgen.TERRAIN_TYPES) {
		cost := fact_row(out, "move_cost", "Move Cost", "number", allocator)
		tabula.add_num(cost, "value", worldgen.TERRAIN_TYPES[terrain].move_cost, allocator)
	}

	tile := board.tile_at(game.board, pos)
	features: [len(defs.Feature)]string
	feature_count := 0
	names := FEATURE_NAMES
	for feature in defs.Feature {
		if tile.features[feature] == 0 {continue}
		features[feature_count] = names[feature]
		feature_count += 1
	}
	// A tile with no feature carries no features row.
	if feature_count > 0 {
		row := fact_row(out, "features", "Features", "text", allocator)
		tabula.add_string(
			row,
			"value",
			strings.join(features[:feature_count], ", ", allocator),
			allocator,
		)
	}

	home := thing.field_ref_get(db, .Home, pos)
	if home != 0 {
		name := thing_name(db, home, allocator)
		if len(name) > 0 {
			row := fact_row(out, "home", "Home", "text", allocator)
			tabula.add_string(row, "value", name, allocator)
		}
	}

	// The groups that reach this tile, and the part of its food that each one
	// takes. A tile that two groups reach gives each of them one half.
	claims := thing.ifield_get(db, pos, .Claims)
	if claims > 0 {
		drawn := fact_row(out, "drawn_by", "Drawn By", "number", allocator)
		tabula.add_num(drawn, "value", f32(claims), allocator)
		share := fact_row(out, "share_each", "Share Each", "number", allocator)
		tabula.add_num(share, "value", display_ratio(1.0 / f32(claims)), allocator)
	}
}

// A thing with no group type adds nothing, so a wagon holds no food row.
@(private)
group_facts :: proc(
	db: ^thing.Db,
	out: ^tabula.Value,
	id: thing.Id,
	allocator := context.allocator,
) {
	type_idx := int(thing.ivar_get(db, id, .Group_Type))
	if type_idx == 0 || type_idx >= len(GROUP_TYPES) {return}
	type := group_type_get(type_idx)

	population := thing.var_get(db, id, .Population)
	food_in := thing.var_get(db, id, .Food_In)
	share := thing.var_get(db, id, .Food_Share)
	taken := thing.var_get(db, id, .Food_Taken)
	drawn := thing.var_get(db, id, .Food_Drawn)

	way := fact_row(out, "way_of_life", "Way of Life", "text", allocator)
	tabula.add_string(way, "value", type.name, allocator)
	people := fact_row(out, "people", "People", "x_of_y", allocator)
	tabula.add_num(people, "value", population, allocator)
	tabula.add_num(people, "limit", type.population_cap, allocator)

	food := fact_row(out, "food", "Food", "balance", allocator)
	tabula.add_num(food, "value", food_in - share - population, allocator)
	tabula.add_string(
		food,
		"hover",
		fmt.aprintf(
			"land %g - granary %g - upkeep %g",
			food_in,
			share,
			population,
			allocator = allocator,
		),
		allocator,
	)

	granary := fact_row(out, "granary", "Granary", "x_of_y", allocator)
	tabula.add_num(granary, "value", thing.var_get(db, id, .Food_Store), allocator)
	tabula.add_num(granary, "limit", type.storage_cap, allocator)
	// The change of this tick, and the part of the share that the granary had
	// no room for. That part is above 0 only where the granary is full.
	change := taken - drawn
	wasted := share - taken
	hover :=
		change == 0 ? "no change this tick" : fmt.aprintf("%+g this tick", change, allocator = allocator)
	if wasted > 0 {
		hover = fmt.aprintf(
			"%s, %g wasted: the granary is full",
			hover,
			wasted,
			allocator = allocator,
		)
	}
	tabula.add_string(granary, "hover", hover, allocator)
}

// The actions that the player can take on this thing. A thing with no group
// type offers no action.
@(private)
group_actions :: proc(
	db: ^thing.Db,
	out: ^tabula.Value,
	id: thing.Id,
	allocator := context.allocator,
) {
	type_idx := int(thing.ivar_get(db, id, .Group_Type))
	if type_idx == 0 || type_idx >= len(GROUP_TYPES) {return}

	actions := tabula.add_object(out, "actions", allocator)
	if thing.flag_get(db, id, .Player) {
		tabula.add_string(actions, "label", "This is the player.", allocator)
	} else {
		tabula.add_string(actions, "label", "This is a target", allocator)
		{
			act := tabula.add_object(actions, "action", allocator)
			tabula.add_string(act, "kind", "talk", allocator)
			tabula.add_string(act, "name", "Talk", allocator)
		}
		{
			act := tabula.add_object(actions, "action", allocator)
			tabula.add_string(act, "kind", "attack", allocator)
			tabula.add_string(act, "name", "Attack", allocator)
		}
	}
}

// The selection is one object: a `name` for the title, a `facts` object that
// holds the rows and the sections, and an `actions` object that holds what
// the player can do.
@(private)
selection_info :: proc(game: ^Game, root: ^tabula.Value, allocator := context.allocator) {
	db := game.db
	selection := game.selection
	if selection.kind == .None {return}
	info_obj := tabula.add_object(root, "selection", allocator)
	facts := tabula.add_object(info_obj, "facts", allocator)
	if selection.kind == .Thing {
		names := SPRITE_NAMES
		sprite := Sprite(clamp(int(thing.ivar_get(db, selection.id, .Sprite)), 0, len(Sprite) - 1))
		name := thing_name(db, selection.id, allocator)
		if len(name) == 0 {name = names[sprite]}
		tabula.add_string(info_obj, "name", name, allocator)
		group_facts(db, facts, selection.id, allocator)
		group_actions(db, info_obj, selection.id, allocator)
	}
	tile := tabula.add_object(facts, "tile", allocator)
	tabula.add_string(tile, "name", "Tile", allocator)
	tile_facts(game, tile, selection.tile, allocator)
}

info :: proc(game: ^Game, allocator := context.allocator) -> ^tabula.Value {
	result := tabula.build_object(allocator)
	selection_info(game, result, allocator)
	return result
}

////////////////////////////////
//~ fp: Economy

// The groups of one tick, as a list on the scratch of that tick. The Project
// phase fills `hits` with the tiles of the disc of the group, and the Economy
// phase reads those tiles again, so one walk of the disc serves both phases.
@(private)
Group_Node :: struct {
	id:   thing.Id,
	type: ^Group_Type,
	hits: []Tile_Hit, // one contiguous block on the scratch of the tick
}

@(private)
Group_List :: chunk.List(Group_Node, 16)

// Each read and each write is of this group, so the tick can run the groups
// in any order.
@(private)
group_economy :: proc(game: ^Game, node: ^Group_Node) {
	db := game.db
	id := node.id
	type := node.type

	// The sum keeps its decimals to the end, so many small yields still come
	// to food.
	food_in: f32 = 0
	for hit in node.hits {
		pos := hit.coords
		claims := thing.ifield_get(db, pos, .Claims)
		if claims <= 0 {continue} 	// this group covers the tile, so the count is 1 or more
		food_in += tile_yield(db, type, pos) / f32(claims)
	}
	food_in = floor_whole(food_in)

	// The share leaves the people either way, room for it or not.
	store := thing.var_get(db, id, .Food_Store)
	share := floor_whole(food_in * type.storage_share)
	taken := min(share, max(type.storage_cap - store, 0.0))
	store += taken

	// One person eats one food.
	population := thing.var_get(db, id, .Population)
	available := food_in - share
	drawn := min(max(population - available, 0.0), store)
	store -= drawn

	if available > population + 1.0 && population < type.population_cap {
		population += 1.0
	}

	// Store and population are state; the four food numbers are the record.
	thing.var_set(db, id, .Food_Store, store)
	thing.var_set(db, id, .Population, population)
	thing.var_set(db, id, .Food_In, food_in)
	thing.var_set(db, id, .Food_Share, share)
	thing.var_set(db, id, .Food_Taken, taken)
	thing.var_set(db, id, .Food_Drawn, drawn)
}

////////////////////////////////
//~ fp: Tick

@(private)
TICK_DT :: f32(0.1) // seconds of sim per tick; every rate below is per-tick
@(private)
TICK_BANK_MAX :: f32(0.5) // at most 5 banked ticks replay after a stall

update :: proc(game: ^Game, dt: f32) {
	db := game.db
	dt := dt * (game.paused ? 0 : 1)
	game.move_timer = min(game.move_timer + dt, TICK_BANK_MAX)
	for game.move_timer > TICK_DT {
		// The allocations of one tick go on the temp allocator, which the
		// frame of the caller frees.
		scratch := context.temp_allocator

		game.tick_num += 1
		game.move_timer -= TICK_DT

		// No phase reads across the things what it also writes across the
		// things, so the order of the slots decides no result.

		// One general pass over the things, so other lists can join it later.
		groups: Group_List
		for this := thing.first(db); this != 0; this = thing.next(db, this) {
			if thing.flag_get(db, this, .Has_Influence) {
				chunk.push(&groups, Group_Node{id = this, type = group_type_of(db, this)}, scratch)
			}
		}

		//- fp: Project -- reads: group positions, types; writes: Claims, Home
		// Claims is a sum, so the order of the groups cannot change it. Home
		// ties to the lower slot, and only the display reads Home.
		{
			world_size := thing.world_size(db)
			cell_count := world_size.x * world_size.y
			// A distance above each distance inside a disc, so the first group
			// that covers a tile always wins the test below.
			DIST_SQ_NONE :: 0x7FFFFFFF
			best_dist_sq := make([]int, cell_count, scratch)
			for i in 0 ..< cell_count {best_dist_sq[i] = DIST_SQ_NONE}
			thing.ifield_clear(db, .Claims)
			thing.field_ref_clear(db, .Home)

			it := chunk.iterator(groups)
			for node in chunk.iterate(&it) {
				if node.type.range <= 0 {continue}
				disc := board.disc(game.board, xy_get(db, node.id), node.type.range)
				hits := make([]Tile_Hit, board.disc_bound(disc), scratch)
				hit_count := 0
				for board.disc_next(&disc) {
					thing.ifield(db, disc.pos, .Claims)^ += 1
					c := disc.pos.y * world_size.x + disc.pos.x
					if disc.dist_sq < best_dist_sq[c] {
						best_dist_sq[c] = disc.dist_sq
						thing.field_ref_set(db, .Home, disc.pos, node.id)
					}
					hits[hit_count] = {disc.pos}
					hit_count += 1
				}
				node.hits = hits[:hit_count]
			}
		}

		//- fp: Economy -- reads: Claims, terrain, types, group facts; writes: group facts
		// The rules are in group_economy. A group reads and writes its own
		// facts only, so the whole economy is one pass in any order.
		{
			it := chunk.iterator(groups)
			for node in chunk.iterate(&it) {
				group_economy(game, node)
			}
		}

		//- fp: Move -- reads: goals, board; writes: positions, move points
		// Last in the tick, so the area of a thing and the position of that
		// thing always agree inside one tick.
		for this := thing.first(db); this != 0; this = thing.next(db, this) {
			if thing.flag_get(db, this, .Placed) && thing.flag_get(db, this, .Mobile) {
				// One point for each tick, which is one step across plains.
				// The store of points has a limit. A thing that waits on cheap
				// ground therefore cannot buy a long move across expensive
				// ground later.
				pts := thing.var(db, this, .Move_Pts)
				pts^ = min(pts^ + 1.0, 4.0)

				// move to the next waypoint while the points permit it
				for {
					pos := xy_get(db, this)
					waypoint := thing.ref_get(db, .Goal, this)
					if waypoint == 0 {break} 	// placed but goalless: stands still
					goal := xy_get(db, waypoint)

					if pos == goal {
						waypoint = thing.ref_get(db, .Next, waypoint)
						thing.ref_set(db, .Goal, this, waypoint)
						goal = xy_get(db, waypoint)
					}

					next_pos := board.path_next_towards(game.board, pos, goal)
					if next_pos == pos {
						// no path from here, so go to the waypoint after it
						thing.ref_set(db, .Goal, this, thing.ref_get(db, .Next, waypoint))
						break
					}

					cost := board.step_cost(game.board, pos, next_pos)
					if cost <= 0 || pts^ < cost {break} 	// not affordable yet
					pts^ -= cost
					xy_set(db, this, next_pos)
				}
			}
		}

		thing.commit(db)
		extract(game)
	}
	selection_refresh(game)
}

////////////////////////////////
//~ fp: Map Items
//
// The map items are what the display layer draws. They carry no meaning of
// the game: a shape does not say that it is a territory, a person or a
// boundary, because the display layer has no use for that and cannot keep it
// correct.
//
// There are two lists, and they part where a real seam is. A shape is a
// finished order to draw. The ground is not: it is what the art system of the
// display layer reads to choose the pieces of a tile, and only that layer
// knows those pieces.
//
// The display layer reads nothing but these lists.

// One tile of the window. A cell outside the board is a ring cell: it takes a
// boundary shape from its neighbours, and draws no ground of its own.
Map_Ground :: struct {
	pos:        geo.V2i,
	neighbours: [9]board.Terrain, // the 3x3 tiles around pos, row by row. A
	// tile off the board reads 0.
	features:   [defs.Feature]u8, // the connection masks at pos
}

// One shape across one tile, above the ground.
//
// The list is in the order of the draw, and a shape covers each shape before
// it. A position can appear more than one time: two things that reach one
// tile give that tile two shapes.
Map_Shape :: struct {
	pos:    geo.V2i,
	color:  geo.V4, // the color of the shape, or the tint of its art. It is the
	// color to draw, with its alpha, and not a color to adjust.
	sprite: Sprite, // the art of the shape. The nil sprite asks for no art, and
	// draws the color across the whole tile.
	id:     thing.Id, // chooses between the variants of the art. It is stable for
	// the life of a thing, so the choice is stable too.
}

// The shapes are an unrolled list, because every reader walks them in order
// and no reader indexes them.
Map_Shape_List :: chunk.List(Map_Shape, 256)

Map_Items :: struct {
	ground:   []Map_Ground, // flat: the window gives its exact size in one multiply
	shapes:   Map_Shape_List,
	// The tile of the selection, which the display layer marks above
	// everything. The mark takes a color of the theme, so the display layer
	// owns how it looks, and it is not a shape.
	has_mark: bool,
	mark:     geo.V2i,
}

Map_Mode_Flag :: enum {
	Pawns,
	Influence,
}

// The mode asks for one ground item at each cell of the window, with both
// corners. The window can go one cell past the edge of the board, to give the
// boundary shapes at that edge an owner. The mode then asks for one surface
// item at each pawn that stands inside the window.
Map_Mode :: struct {
	window: geo.Rng2i,
	flags:  bit_set[Map_Mode_Flag],
}

// How much of the ground below a claim a person sees. A shape carries the
// color to draw, so the strength of a claim is here and in no other place.
// Two claims on one tile add together, and that tile becomes darker.
@(private)
CLAIM_ALPHA :: f32(0.35)

map_items :: proc(game: ^Game, mode: Map_Mode, allocator := context.allocator) -> Map_Items {
	window := mode.window

	db := game.db
	b := game.board
	out: Map_Items
	if geo.rng2i_is_empty(window) {return out}

	pawns := board.pawns_all(b, context.temp_allocator)

	//- fp: the ground: one cell at each tile of the window, with the ring of
	//  tiles outside the board
	out.ground = make([]Map_Ground, geo.rng2i_area(window), allocator)
	ground_count := 0
	for y := window.min.y; y <= window.max.y; y += 1 {
		for x := window.min.x; x <= window.max.x; x += 1 {
			cell := &out.ground[ground_count]
			ground_count += 1
			cell.pos = {x, y}
			for dy := -1; dy <= 1; dy += 1 {
				for dx := -1; dx <= 1; dx += 1 {
					cell.neighbours[(dy + 1) * 3 + (dx + 1)] =
						board.tile_at(b, cell.pos + geo.V2i{dx, dy}).terrain
				}
			}
			tile := board.tile_at(b, cell.pos)
			cell.features = tile.features
		}
	}

	//- fp: the shapes above the ground, in the order of their draw. A claim
	//  goes down first, so a thing that stands on a tile covers the claims of
	//  it.

	// One shape for each claim that a group holds on a tile of the window. Two
	// groups that reach one tile give that tile two shapes, so the count of
	// the shapes is the count of the claims and not of the tiles. The claims
	// come from the database at this call, so they always agree with the last
	// tick.
	if .Influence in mode.flags {
		for this := thing.first(db); this != 0; this = thing.next(db, this) {
			if !thing.flag_get(db, this, .Has_Influence) {continue}
			type := group_type_of(db, this)
			if type.range <= 0 {continue}
			for it := board.disc(b, xy_get(db, this), type.range); board.disc_next(&it); {
				if !geo.rng2i_contains(window, it.pos) {continue}
				shape := chunk.push(&out.shapes, Map_Shape{}, allocator)
				shape.pos = it.pos
				shape.color = color.rgb_from_hash(u64(this))
				shape.color.a = CLAIM_ALPHA
			}
		}
	}

	// One shape for each pawn that stands inside the window.
	if .Pawns in mode.flags {
		for pawn in pawns {
			if !geo.rng2i_contains(window, pawn.pos) {continue}
			shape := chunk.push(&out.shapes, Map_Shape{}, allocator)
			this := thing.Id(pawn.key)
			shape.id = this
			shape.pos = pawn.pos
			shape.color = {1, 1, 1, 1}
			if thing.flag_get(db, this, .Debug) {
				shape.color = {1, 0, 0, 1}
			}
			shape.sprite = Sprite(
				clamp(int(thing.ivar_get(db, this, .Sprite)), 0, len(Sprite) - 1),
			)
		}
	}

	//- fp: the mark of the selection
	if game.selection.kind != .None && geo.rng2i_contains(window, game.selection.tile) {
		out.has_mark = true
		out.mark = game.selection.tile
	}

	return out
}


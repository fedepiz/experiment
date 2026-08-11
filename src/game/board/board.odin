package board

import "core:math"
import "core:mem"

import "../../geo"
import "../../rng"
import "../defs"

////////////////////////////////
//~ fp: Board
//
// The game board is a derived spatial index. It is never the authority. The
// truth is in the thing database, in its fields and its things. The board
// holds a copy of that truth: one terrain for each tile, features as
// connections between tiles, and pawns that stand on tiles. The shape of the
// copy suits spatial questions and pathfinding. You can compute the copy
// again from the truth at any time.
//
// The caller that writes the copy uses tile_at, then clears the path cache.
//
// The board knows nothing more of the game. A terrain kind, a pawn kind and a
// pawn identity are numbers with no meaning here. The game gives them a
// meaning elsewhere.
//
// A coordinate is an integer tile position. Tile (0,0) is the top left corner,
// x increases to the right and y increases downward.
//
// All reads are total. A position out of bounds and an unknown handle both
// resolve to a shared nil object, which is read-only by convention. A chain of
// queries therefore cannot crash. Writes to pawns keep bookkeeping, so they go
// through functions. Those functions do nothing for a nil object or a position
// out of bounds.

////////////////////////////////
//~ fp: Tiles
//
// A tile is one cell of the grid. Its terrain is a number with no meaning to
// the board. The board uses the number only as an index into the travel
// costs. The pawn place and pawn remove functions write the pawn list. Any
// caller can read it.
//
// A feature at a tile is a connection mask. Bit d is set when the feature
// continues toward neighbour d. A feature is present at a tile when its mask
// is not 0. A feature always connects: an object that stands at one tile and
// points in no direction is a pawn, and not a feature.

Terrain :: u16

Tile :: struct {
	terrain: Terrain,
	features: [defs.Feature]u8, // connection masks, bit d = toward Dir4 d

	// the pawns that stand here. Pawn place and pawn remove keep this list.
	first_pawn: ^Pawn,
	last_pawn: ^Pawn,
}

////////////////////////////////
//~ fp: Pawns
//
// A pawn is an object that stands on a tile: a settlement, an army, a marker.
// The board gives a pawn a position and nothing more.
//
// The board does not make pawn identities. The caller supplies a u64 key,
// which has no meaning here. The game supplies its thing ids. pawn_place
// writes the position of a known key and makes a pawn for an unknown key, so
// one call does both creation and movement. A lookup of an unknown key gives
// the nil pawn.

Pawn :: struct {
	next: ^Pawn, // in the pawn list of its tile; in the free list when removed
	prev: ^Pawn,
	hash_next: ^Pawn, // in the chain of its key bucket

	key: u64, // the identity that the caller gave this pawn. The board indexes
	          // the key and reads nothing from it. The caller answers every
	          // other question about the pawn from the key.
	pos: geo.V2i,
}

////////////////////////////////
//~ fp: Travel Rules
//
// The travel rules say how a movement cost reads the map. The board owns
// them, at board.rules. Write them one time. Every path query then agrees on
// the meaning of movement.
//
// The zero rules are valid (ZII). Each terrain then costs 1, and roads and
// rivers change nothing.

TERRAIN_CAP :: 32

Travel_Rules :: struct {
	// The cost to enter a tile of terrain t is terrain_cost[t]. A cost of 0 or
	// less makes the terrain impassable. A terrain id past the table is
	// impassable. An empty table makes each terrain cost 1.
	//
	// The costs are in the structure and not behind a pointer. They are a copy
	// of the terrain data, so the A* search stays inside the board.
	terrain_cost: [dynamic;TERRAIN_CAP]f32,

	// A road cost of more than 0 replaces the terrain cost of a step that
	// follows a road connection. Such a step also pays no river crossing cost,
	// because a road across a river is a bridge or a ford.
	road_cost: f32,

	// A river crossing cost of more than 0 is added to each step that enters a
	// tile with a river.
	river_cross_cost: f32,
}

////////////////////////////////
//~ fp: Path Cache
//
// The cache keeps each path that the board computes, in an array of a fixed
// size. A search of the array is linear. The cache stops a repeated query from
// a second A* search. path_next_towards is such a query: a caller uses it on
// each turn while an object walks.
//
// The hops of every path are in one shared pool of points. The board drops the
// whole cache when the array fills or the pool fills. It then builds the cache
// again on demand.
//
// The cache does not see a change to the map. Call path_cache_clear after you
// write a tile or a rule. If you do not, the cache gives you an old path. The
// mirror of the game makes this call.

Path_Entry :: struct {
	from: geo.V2i,
	to: geo.V2i,
	first: int, // the first hop, in the point pool of the board
	count: int, // hops, with both ends; a count of 0 means "no path"
	cost: f32,
}

////////////////////////////////
//~ fp: Board State

Board :: struct {
	allocator: mem.Allocator, // the memory of the board: tiles, pawns and the path cache
	width: int,
	height: int,
	tiles: []Tile, // width * height tiles, row by row

	rules: Travel_Rules,

	first_free_pawn: ^Pawn,  // removed pawns, which place uses again
	pawn_count: int,         // the pawns that stand on the board
	pawn_buckets: []^Pawn,   // key to pawn, chained by hash_next
	                         // (a power-of-two count, set at allocation)

	//- fp: the path cache. Its capacity is set at allocation.
	entries: []Path_Entry,
	entry_count: int,
	points: []geo.V2i, // the shared pool of hops. Each entry points into it.
	point_count: int,  // its capacity is width * height, so one path always fits
}

////////////////////////////////
//~ fp: Nil
//
// A lookup out of bounds and a lookup of an unknown key resolve to one of
// these shared objects. A zeroed object is a nil object (ZII), so they need no
// initialization. They are read-only by convention. Never write through a nil
// object.

nil_tile: Tile
nil_pawn: Pawn

////////////////////////////////
//~ fp: Grid

alloc :: proc(allocator: mem.Allocator, width, height: int, path_cache_entries: int) -> ^Board {
	assert(width > 0 && height > 0 && path_cache_entries > 0)
	b := new(Board, allocator)
	b.allocator = allocator
	b.width = width
	b.height = height
	tile_count := width * height
	b.tiles = make([]Tile, tile_count, allocator)
	b.entries = make([]Path_Entry, path_cache_entries, allocator)
	b.points = make([]geo.V2i, tile_count, allocator)
	// About 4 tiles for each bucket keeps the pawn chains short at any normal
	// density. A larger density makes the chains longer, and the table does not
	// fill up.
	bucket_count := 64
	for bucket_count * 4 < tile_count { bucket_count *= 2 }
	b.pawn_buckets = make([]^Pawn, bucket_count, allocator)
	return b
}

in_bounds :: proc(b: ^Board, p: geo.V2i) -> bool {
	return 0 <= p.x && p.x < b.width && 0 <= p.y && p.y < b.height
}

tile_at :: proc(b: ^Board, p: geo.V2i) -> ^Tile { // out of bounds: the nil tile
	result := &nil_tile
	if in_bounds(b, p) {
		result = &b.tiles[p.y * b.width + p.x]
	}
	return result
}

//- fp: distances in tile space. These need no board.

distance_steps :: proc(a, b: geo.V2i) -> int { // moves with 4-way movement (manhattan)
	return abs(a.x - b.x) + abs(a.y - b.y)
}

distance :: proc(a, b: geo.V2i) -> f32 { // euclidean, in a straight line
	dx := f32(a.x - b.x)
	dy := f32(a.y - b.y)
	return math.sqrt(dx * dx + dy * dy)
}

////////////////////////////////
//~ fp: Disc Walks
//
// A disc walk gives you the tiles of the board within a euclidean radius of a
// center. Each question about a range needs this shape, so the board supplies
// it one time. The protocol is the protocol of the iterators of the thing
// database: construct, then loop on next.
//
//   for it := board.disc(b, center, range); board.disc_next(&it); {
//     ... it.pos, it.dist_sq ...
//   }
//
// The walk gives the tiles row by row, over the bounding box that it clamps to
// the board. Both radii are inclusive, as in the partition. Both must be 0 or
// more, which an assert checks. A center off the board and an empty band both
// give zero tiles, so no caller needs a guard for them.

Disc :: struct {
	pos: geo.V2i, // the tile of the walk
	dist_sq: int, // its squared distance from the center. Take the square root
	              // only when you need the true distance. A walk that does not
	              // need it pays nothing.

	//- fp: internals
	center: geo.V2i,
	x0, y0, x1, y1: int, // the bounding box, clamped to the board
	min_sq, max_sq: f32, // the band, squared
}

// a ring: the tiles at radius_min or more from the center
disc_ring :: proc(b: ^Board, center: geo.V2i, radius_min, radius_max: f32) -> Disc {
	it: Disc
	it.center = center
	// The square of a radius has no sign, so this function cannot read a
	// negative radius. A negative radius is a mistake of the caller.
	assert(radius_min >= 0 && radius_max >= 0)
	it.min_sq = radius_min * radius_min
	it.max_sq = radius_max * radius_max

	// The walk reaches the bounding box of the outer radius only, so no walk
	// reads the whole board. The cast to an integer is exact here: with an
	// integer center, the largest |dx| in the band is floor(radius_max).
	reach := int(radius_max)
	it.x0 = max(center.x - reach, 0)
	it.y0 = max(center.y - reach, 0)
	it.x1 = min(center.x + reach, b.width - 1)
	it.y1 = min(center.y + reach, b.height - 1)

	// one position before the first tile, so the first call to next reaches it
	it.pos = {it.x0 - 1, it.y0}
	return it
}

disc :: proc(b: ^Board, center: geo.V2i, radius: f32) -> Disc {
	return disc_ring(b, center, 0, radius)
}

disc_next :: proc(it: ^Disc) -> bool {
	for {
		it.pos.x += 1
		if it.pos.x > it.x1 {
			it.pos.x = it.x0
			it.pos.y += 1
		}
		// After the walk moves to the next row, an x that is past the end shows
		// that the box is empty on that axis. This one test therefore stops a
		// walk that is complete and a walk that never had a tile.
		if it.pos.x > it.x1 || it.pos.y > it.y1 { return false }

		dx := it.pos.x - it.center.x
		dy := it.pos.y - it.center.y
		it.dist_sq = dx * dx + dy * dy
		dist_sq := f32(it.dist_sq)
		if dist_sq <= it.max_sq && dist_sq >= it.min_sq { return true }
	}
}

// The largest number of tiles that a walk can give, which is its bounding box.
// Use it when you must allocate before you walk. An empty walk gives 0.
disc_bound :: proc(it: Disc) -> int {
	result := 0
	if it.x0 <= it.x1 && it.y0 <= it.y1 {
		result = (it.x1 - it.x0 + 1) * (it.y1 - it.y0 + 1)
	}
	return result
}

////////////////////////////////
//~ fp: Features
//
// The masks are a copy of data that the board does not own. The writer that
// makes the copy keeps this rule: a connection from p toward d is also a
// connection from the neighbour of p toward the opposite of d. See
// worldgen.field_connect.

feature_mask :: proc(b: ^Board, p: geo.V2i, feature: defs.Feature) -> u8 { // present when the mask is not 0
	return tile_at(b, p).features[feature]
}

////////////////////////////////
//~ fp: Pawns
//
// To read the pawns of one tile, start at tile_at(...).first_pawn, then
// follow .next.

@(private)
pawn_bucket :: proc(b: ^Board, key: u64) -> ^^Pawn {
	// Mix the key first. Keys that follow each other would fill one bucket.
	return &b.pawn_buckets[rng.mix_u64(key) & u64(len(b.pawn_buckets) - 1)]
}

pawn_lookup :: proc(b: ^Board, key: u64) -> ^Pawn { // unknown key: the nil pawn
	pawn := pawn_bucket(b, key)^
	for pawn != nil && pawn.key != key { pawn = pawn.hash_next }
	return pawn != nil ? pawn : &nil_pawn
}

// place writes the position of a known key, and makes a pawn for an unknown
// key. It does nothing when the position is out of bounds.
pawn_place :: proc(b: ^Board, key: u64, pos: geo.V2i) {
	dst := tile_at(b, pos)
	if dst == &nil_tile { return }
	pawn := pawn_lookup(b, key)
	if pawn != &nil_pawn {
		src := tile_at(b, pawn.pos) // placed pawns are always in bounds
		if pawn.prev != nil { pawn.prev.next = pawn.next } else { src.first_pawn = pawn.next }
		if pawn.next != nil { pawn.next.prev = pawn.prev } else { src.last_pawn = pawn.prev }
	} else {
		pawn = b.first_free_pawn
		if pawn != nil {
			b.first_free_pawn = pawn.next
			pawn^ = {}
		} else {
			pawn = new(Pawn, b.allocator)
		}
		pawn.key = key
		bucket := pawn_bucket(b, key)
		pawn.hash_next = bucket^
		bucket^ = pawn
		b.pawn_count += 1
	}
	pawn.pos = pos
	if dst.first_pawn == nil {
		dst.first_pawn = pawn
		dst.last_pawn = pawn
		pawn.next = nil
		pawn.prev = nil
	} else {
		pawn.prev = dst.last_pawn
		pawn.next = nil
		dst.last_pawn.next = pawn
		dst.last_pawn = pawn
	}
}

pawn_remove :: proc(b: ^Board, key: u64) {
	link := pawn_bucket(b, key)
	for link^ != nil && link^.key != key { link = &link^.hash_next }
	pawn := link^
	if pawn == nil { return }
	link^ = pawn.hash_next
	tile := tile_at(b, pawn.pos) // placed pawns are always in bounds
	if pawn.prev != nil { pawn.prev.next = pawn.next } else { tile.first_pawn = pawn.next }
	if pawn.next != nil { pawn.next.prev = pawn.prev } else { tile.last_pawn = pawn.prev }
	pawn.next = b.first_free_pawn
	b.first_free_pawn = pawn
	b.pawn_count -= 1
}

// each pawn that stands in [min, max], with both corners, pushed on `allocator`
pawns_in_rect :: proc(b: ^Board, min_p, max_p: geo.V2i, allocator := context.allocator) -> []^Pawn {
	x0 := max(min_p.x, 0)
	y0 := max(min_p.y, 0)
	x1 := min(max_p.x, b.width - 1)
	y1 := min(max_p.y, b.height - 1)
	// Count, then fill. Two small passes are less work than storage that grows.
	count := 0
	for y := y0; y <= y1; y += 1 {
		for x := x0; x <= x1; x += 1 {
			tile := &b.tiles[y * b.width + x]
			for pawn := tile.first_pawn; pawn != nil; pawn = pawn.next {
				count += 1
			}
		}
	}
	result := make([]^Pawn, count, allocator)
	n := 0
	for y := y0; y <= y1; y += 1 {
		for x := x0; x <= x1; x += 1 {
			tile := &b.tiles[y * b.width + x]
			for pawn := tile.first_pawn; pawn != nil; pawn = pawn.next {
				result[n] = pawn
				n += 1
			}
		}
	}
	return result
}

// each pawn on the board, pushed on `allocator`. The order is not defined.
pawns_all :: proc(b: ^Board, allocator := context.allocator) -> []^Pawn {
	result := make([]^Pawn, b.pawn_count, allocator)
	n := 0
	for bucket in b.pawn_buckets {
		for pawn := bucket; pawn != nil; pawn = pawn.hash_next {
			result[n] = pawn
			n += 1
		}
	}
	return result
}

////////////////////////////////
//~ fp: Terrain Queries
//
// A patch is a rectangle of terrain, copied out row by row. The board clamps
// the rectangle that you ask for. `min`, `width` and `height` then report the
// part that the patch covers. A rectangle that goes past an edge therefore
// comes back smaller, and the board adds no padding. A width or a height of 0
// means that the rectangle missed the board.

Terrain_Patch :: struct {
	min: geo.V2i, // the top left tile of the patch
	width: int,   // the size of the patch. elems holds width * height ids.
	height: int,
	elems: []Terrain, // row by row: elems[y * width + x] is the terrain at min + (x, y)
}

// the terrain in [min, max], with both corners, pushed on `allocator`
terrain_in_rect :: proc(b: ^Board, min_p, max_p: geo.V2i, allocator := context.allocator) -> Terrain_Patch {
	result: Terrain_Patch
	x0 := max(min_p.x, 0)
	y0 := max(min_p.y, 0)
	x1 := min(max_p.x, b.width - 1)
	y1 := min(max_p.y, b.height - 1)
	if x0 > x1 || y0 > y1 { return result }
	result.min = {x0, y0}
	result.width = x1 - x0 + 1
	result.height = y1 - y0 + 1
	result.elems = make([]Terrain, result.width * result.height, allocator)
	for y := y0; y <= y1; y += 1 {
		for x := x0; x <= x1; x += 1 {
			result.elems[(y - y0) * result.width + (x - x0)] = b.tiles[y * b.width + x].terrain
		}
	}
	return result
}

////////////////////////////////
//~ fp: Pathfinding -- A* core
//
// The state of the search is on the temp allocator. The path is the only
// result that leaves the function.
//
// The open set is a binary heap that deletes late. When the search finds a
// better cost for a node, it pushes that node again and does not change the
// entry that is in the heap. It then ignores each entry that it pops with a
// cost that is no longer the best. The in-degree of the grid limits the number
// of pushes, so the heap array holds 4 * tiles + 1 entries from the start and
// never grows.

@(private)
Path_Node :: struct {
	f: f32,
	g: f32, // g at push time; the pop is stale when it no longer matches best_g
	idx: u32,
}

@(private)
heap_push :: proc(heap: []Path_Node, count: ^int, node: Path_Node) {
	at := count^
	count^ += 1
	heap[at] = node
	for at > 0 {
		parent := (at - 1) / 2
		if heap[parent].f <= heap[at].f { break }
		heap[parent], heap[at] = heap[at], heap[parent]
		at = parent
	}
}

@(private)
heap_pop :: proc(heap: []Path_Node, count: ^int) -> Path_Node {
	result := heap[0]
	count^ -= 1
	heap[0] = heap[count^]
	for at := 0; true; {
		left := 2 * at + 1
		right := 2 * at + 2
		smallest := at
		if left < count^ && heap[left].f < heap[smallest].f { smallest = left }
		if right < count^ && heap[right].f < heap[smallest].f { smallest = right }
		if smallest == at { break }
		heap[at], heap[smallest] = heap[smallest], heap[at]
		at = smallest
	}
	return result
}

// The cost of a step from `from_tile` toward `dir` into `to_tile`, under the
// rules of the board. A cost of 0 or less means that the step is not possible.
@(private)
tile_step_cost :: proc(b: ^Board, from_tile: ^Tile, dir: geo.Dir4, to_tile: ^Tile) -> f32 {
	rules := &b.rules
	cost: f32 = 1.0
	if len(rules.terrain_cost) != 0 {
		if int(to_tile.terrain) >= len(rules.terrain_cost) { return 0 }
		cost = rules.terrain_cost[to_tile.terrain]
		if cost <= 0 { return 0 }
	}
	on_road := (from_tile.features[.Road] >> uint(dir)) & 1 != 0
	if on_road && rules.road_cost > 0 {
		cost = rules.road_cost
	} else if to_tile.features[.River] != 0 && rules.river_cross_cost > 0 {
		cost += rules.river_cross_cost
	}
	return cost
}

// The smallest cost that one step can have. The A* estimate uses it, and the
// estimate must never be too large. The result is 0 when the rules make every
// terrain impassable.
@(private)
min_step_cost :: proc(rules: ^Travel_Rules) -> f32 {
	result: f32 = 1.0
	if len(rules.terrain_cost) != 0 {
		result = 0
		for cost in rules.terrain_cost {
			if cost > 0 && (result == 0 || cost < result) { result = cost }
		}
		if result == 0 { return 0 }
	}
	if rules.road_cost > 0 && rules.road_cost < result { result = rules.road_cost }
	return result
}

@(private)
heuristic :: proc(a, b: geo.V2i, min_cost: f32) -> f32 {
	return f32(distance_steps(a, b)) * min_cost // manhattan
}

// the waypoints from `from` to `to`, with both ends. A count of 0 means that
// there is no path.
Path :: struct {
	points: []geo.V2i,
	cost: f32, // the total cost, under board.rules
}

// Search with A*, under board.rules. The path goes on `allocator`. An empty
// points slice means that there is no path.
@(private)
path_compute :: proc(b: ^Board, from, to: geo.V2i, allocator: mem.Allocator) -> Path {
	result: Path
	if !in_bounds(b, from) || !in_bounds(b, to) { return result }
	if from == to {
		result.points = make([]geo.V2i, 1, allocator)
		result.points[0] = from
		return result
	}
	rules := &b.rules
	min_cost := min_step_cost(rules)
	if min_cost <= 0 { return result }

	w := b.width
	tile_count := w * b.height
	scratch := context.temp_allocator

	best_g := make([]f32, tile_count, scratch)
	parent := make([]u32, tile_count, scratch)
	state := make([]u8, tile_count, scratch) // 0 unseen, 1 open, 2 closed
	heap := make([]Path_Node, tile_count * 4 + 1, scratch)
	heap_count := 0

	start := u32(from.y * w + from.x)
	goal := u32(to.y * w + to.x)
	best_g[start] = 0
	parent[start] = start
	state[start] = 1
	heap_push(heap, &heap_count, {heuristic(from, to, min_cost), 0, start})

	found := false
	for heap_count > 0 {
		node := heap_pop(heap, &heap_count)
		if state[node.idx] == 2 || node.g != best_g[node.idx] { continue } // stale duplicate
		state[node.idx] = 2
		if node.idx == goal {
			found = true
			break
		}

		p := geo.V2i{int(node.idx) % w, int(node.idx) / w}
		tile := &b.tiles[node.idx]
		for dir in geo.Dir4 {
			np := p + geo.dir4_delta(dir)
			if !in_bounds(b, np) { continue }
			nidx := u32(np.y * w + np.x)
			if state[nidx] == 2 { continue }
			step := tile_step_cost(b, tile, dir, &b.tiles[nidx])
			if step <= 0 { continue }
			g := node.g + step
			if state[nidx] == 0 || g < best_g[nidx] {
				best_g[nidx] = g
				parent[nidx] = node.idx
				state[nidx] = 1
				heap_push(heap, &heap_count, {g + heuristic(np, to, min_cost), g, nidx})
			}
		}
	}

	if found {
		count := 1
		for idx := goal; idx != start; idx = parent[idx] { count += 1 }
		result.points = make([]geo.V2i, count, allocator)
		result.cost = best_g[goal]
		at := count
		for idx := goal;; idx = parent[idx] {
			at -= 1
			result.points[at] = {int(idx) % w, int(idx) / w}
			if idx == start { break }
		}
	}
	return result
}

// The cost of one step from `from` to the adjacent tile `to`, under
// board.rules. The search pays this same cost for that hop, so an object that
// spends a budget of movement agrees with the route that A* chooses. A cost of
// 0 or less means that the step is not possible: the tile is out of bounds, or
// it is not adjacent, or it is impassable.
step_cost :: proc(b: ^Board, from, to: geo.V2i) -> f32 {
	result: f32 = 0
	if in_bounds(b, from) && in_bounds(b, to) {
		if dir, ok := geo.dir4_from_delta(to - from); ok {
			result = tile_step_cost(b, tile_at(b, from), dir, tile_at(b, to))
		}
	}
	return result
}

// Can an object enter the tile at `p`? This is the terrain part of step_cost,
// as a yes or a no. A position out of bounds gives false.
tile_passable :: proc(b: ^Board, p: geo.V2i) -> bool {
	result := in_bounds(b, p)
	if result && len(b.rules.terrain_cost) != 0 {
		terrain := tile_at(b, p).terrain
		result = int(terrain) < len(b.rules.terrain_cost) &&
		         b.rules.terrain_cost[terrain] > 0
	}
	return result
}

// The passable tile that is nearest to `want`. The board searches outward,
// one ring at a time. It gives `want` itself when the whole board is
// impassable.
snap_passable :: proc(b: ^Board, want: geo.V2i) -> geo.V2i {
	max_radius := max(b.width, b.height)
	for radius in 0 ..< max_radius {
		for dy := -radius; dy <= radius; dy += 1 {
			for dx := -radius; dx <= radius; dx += 1 {
				if abs(dx) != radius && abs(dy) != radius { continue } // ring, not disc
				p := geo.V2i{want.x + dx, want.y + dy}
				if tile_passable(b, p) { return p }
			}
		}
	}
	return want
}

////////////////////////////////
//~ fp: Pathfinding -- cache & public queries

path_cache_clear :: proc(b: ^Board) {
	b.entry_count = 0
	b.point_count = 0
}

// Find the entry for (from, to), or compute it and put it in the cache. The
// result is nil when either end is out of bounds. The pointer is valid until
// the next path query only, because that query can drop the whole cache to
// make space.
@(private)
path_entry_lookup :: proc(b: ^Board, from, to: geo.V2i) -> ^Path_Entry {
	if !in_bounds(b, from) || !in_bounds(b, to) { return nil }
	for idx in 0 ..< b.entry_count {
		entry := &b.entries[idx]
		if entry.from == from && entry.to == to { return entry }
	}

	path := path_compute(b, from, to, context.temp_allocator)
	if b.entry_count >= len(b.entries) ||
	   b.point_count + len(path.points) > len(b.points) {
		path_cache_clear(b) // full: drop everything, rebuild on demand
	}
	entry := &b.entries[b.entry_count]
	b.entry_count += 1
	entry.from = from
	entry.to = to
	entry.first = b.point_count
	entry.count = len(path.points)
	entry.cost = path.cost
	copy(b.points[b.point_count:], path.points)
	b.point_count += len(path.points)
	return entry
}

path_find :: proc(b: ^Board, from, to: geo.V2i, allocator := context.allocator) -> Path {
	result: Path
	entry := path_entry_lookup(b, from, to)
	if entry != nil && entry.count > 0 {
		// Copy the points. Do not point at the cache: a later query can drop it.
		result.points = make([]geo.V2i, entry.count, allocator)
		result.cost = entry.cost
		copy(result.points, b.points[entry.first:][:entry.count])
	}
	return result
}

// The tile after `from` on the path to `to`. The board gives `from` itself
// when the object is at `to` already, and when there is no path. Compare the
// result with `from` to find out that the object does not move.
path_next_towards :: proc(b: ^Board, from, to: geo.V2i) -> geo.V2i {
	result := from
	entry := path_entry_lookup(b, from, to)
	if entry != nil && entry.count > 1 {
		result = b.points[entry.first + 1]
	}
	return result
}

////////////////////////////////
//~ fp: Partition
//
// A partition divides the board between weighted sources. Each source sends
// out a strength that decreases with distance. Each tile goes to the source
// that reaches it with the largest strength.
//
// A source is a pawn key. It sends its strength from the tile where that pawn
// stands. A key that the board does not know sends nothing.
//
// The distance is euclidean, and it ignores the terrain. A strength pays no
// travel cost, and impassable ground does not stop it.

Falloff :: enum {
	None,      // the full strength to the range, and 0 past it
	Linear,    // strength * (1 - d/range)
	Quadratic, // strength * (1 - d/range)^2
}

Source :: struct {
	key: u64,      // the pawn that sends this strength. An unknown key sends nothing.
	range: f32,    // the tiles that it reaches, inclusive. A range of 0 reaches
	               // its own tile only. A range of less than 0 reaches nothing.
	strength: f32, // the strength at the tile of the source. A strength of 0 or
	               // less reaches nothing.
	falloff: Falloff,
}

// The source that holds a tile. A tile reads back unassigned only when no
// source reaches it. When two sources reach a tile with an equal strength, a
// hash of the key against the tile chooses between them. A border therefore
// alternates between its two sources, tile by tile, and no tile falls to
// nobody. The hash reads the key and the tile and nothing more, so the same
// sources always give the same partition, in any order.
Partition_Cell :: struct {
	key: u64,      // key_unassigned only when no source reached this tile
	strength: f32, // the strength that won. It is 0 when the tile is unassigned.
}

// The strength of `source` at `dist` tiles away. The result is 0 past its
// range. The falloff runs to range + 1, so the last tile in the range still
// reads more than 0.
@(private)
source_at :: proc(source: ^Source, dist: f32) -> f32 {
	if dist > source.range { return 0 }
	t := dist / (source.range + 1.0)
	result := source.strength
	switch source.falloff {
	case .Linear:
		result *= 1.0 - t
	case .Quadratic:
		falloff := 1.0 - t
		result *= falloff * falloff
	case .None:
		// flat out to the range
	}
	return result
}

// The rank of a key at one tile, for the case where the strength cannot
// separate two sources. The lowest rank wins. The rank comes from the key and
// the tile and nothing more. The winner is therefore the same in any order of
// the sources, and the same at each new computation.
@(private)
partition_tiebreak :: proc(key: u64, idx: int) -> u64 {
	return rng.hash_u64(u64(idx), key)
}

// Divide every tile between `sources`. The result is width * height cells,
// pushed on `allocator`, row by row: result[y * board.width + x].
partition :: proc(b: ^Board, sources: []Source, key_unassigned: u64, allocator := context.allocator) -> []Partition_Cell {
	tile_count := b.width * b.height
	result := make([]Partition_Cell, tile_count, allocator)
	for &pcell in result {
		pcell = {key = key_unassigned}
	}

	for &source in sources {
		if source.range < 0 || source.strength <= 0 { continue } // range 0 still claims its own tile
		pawn := pawn_lookup(b, source.key)
		if pawn == &nil_pawn { continue }

		// The walk gives the tiles in the range and no others, so the falloff
		// below reads only the tiles that can give more than 0.
		for it := disc(b, pawn.pos, source.range); disc_next(&it); {
			strength := source_at(&source, math.sqrt(f32(it.dist_sq)))
			if strength <= 0 { continue }
			idx := it.pos.y * b.width + it.pos.x
			// The strength decides. Two equal strengths go to the rank of the
			// tile, so a border alternates between its two sources.
			if strength > result[idx].strength ||
			   (strength == result[idx].strength &&
			    partition_tiebreak(source.key, idx) <
			        partition_tiebreak(result[idx].key, idx)) {
				result[idx].key = source.key
				result[idx].strength = strength
			}
		}
	}
	return result
}

package tiling

import "../../geo"
import "../../rng"

////////////////////////////////
//~ fp: Tiling -- the appearance of a map, over classes of cells
//
// The caller declares its classes, which are terrains in this game. It then
// registers its art by id. An id is a u32 that the caller makes, and it has no
// meaning here. In this game an id is a slot of the sprite table.
//
// A registration is one fact, and the calls can come in any order. "The ground
// window (2,3) of this class is id 17" is one such fact, and "case 6 has this
// mask" is another. Each call writes one entry of a table. A second call with
// the same fact writes over the first. No set of facts must be complete, and
// no order is necessary.
//
// For each cell, the caller then gives the 3x3 window of classes around that
// cell. It receives the pieces to draw: an art id, a mask id that can be
// absent, a layer, and an offset.
//
// This module holds each rule of appearance: the index into the ground of a
// torus, the choice between variants, the density of the overlays, the shape
// at a border, and the boundaries of the dual grid. The module sees no pixel,
// and the caller sees no rule.
//
// cell is pure. The same config, the same window and the same position give
// the same answer. It holds no state, and it reads no asset and no board.
//
//- fp: The map draws the ground on the dual grid. That grid is half a cell
//  away from the map grid, so the center of each dual cell is a point where
//  four map cells meet.
//
//  A dual cell paints its own area in full. Nothing paints outside its cell,
//  so no piece covers another piece. Each map cell owns the dual cell at its
//  north west corner, at the offset {-0.5,-0.5}. The draw loop of the caller
//  must go one row and one column past the right edge and the bottom edge of
//  the view, so that each dual cell at an edge has an owner.
//
//  Inside a dual cell, the classes stack in the order that they cover each
//  other. The class with the lowest rank draws its ground window in full, as
//  if there were no border. A cell with one class is that piece alone.
//
//  Each class with a higher rank then draws its own ground through an alpha
//  mask. The shape of that mask covers the corners whose class has a rank at
//  or above the rank of this class. Two classes with an equal rank compare
//  their class ids.
//
//  A contour therefore stays inside the contour below it, and two contours
//  never cross. Contours also meet across two dual cells, because of how this
//  module builds them.
//
//  A transition is never one piece of art. It is always a ground id with a
//  mask id, and the renderer multiplies the two. The art therefore stays in
//  parts. The case of a masked layer is always one of the 14 cases below. A
//  class with no ground has nothing to spill, so the module skips it.
//
//  The module cuts a ground window on the offset grid. Window (gx,gy) is the
//  crop of the torus at (gx+0.5, gy+0.5) cells. The window of a dual cell is
//  therefore the part of the world under that cell. Two adjacent cells, with
//  one class or with more, continue one texture for each class with no seam.
//
//- fp: The cases, which are the index of the mask table. A case is a code of
//  4 bits, which are the CORNER_* bits. The 14 cases become 4 shapes under
//  rotation, and those 4 shapes are the mask art that a generator makes.
//
//   corner  1,2,4,8    one corner is covered. The contour is a quarter circle.
//   half    3,5,10,12  two adjacent corners. The contour is near to straight.
//   saddle  6,9        two opposite corners, joined by a diagonal neck.
//                      Movement has 8 directions, and the art agrees.
//   inner   7,11,13,14 three corners. It is the opposite of a quarter circle.
//
//  The rule for the generator of the masks: a contour enters and leaves a dual
//  cell at the middle of an edge. Between those two points the contour can
//  take any path. This rule is what makes the contours of two adjacent dual
//  cells meet.
//
//- fp: A network is a line feature, such as a river or a road. The module
//  chooses its art from a connection mask of 4 bits, where bit d is the
//  direction Dir4 d. The board supplies that mask, and the module does not
//  change it.
//
//  The art of a network is one finished piece for each connection case. A
//  piece enters and leaves at the middle of an edge, so two pieces join across
//  two cells.
//
//  The module draws the networks in the order of their ids, so a road above a
//  river reads as a bridge. A cell with any network draws no overlay, because
//  nothing stands on a river or on a road.
//
//- fp: The rule for the caller that draws. A piece can cover a part of an
//  adjacent cell, because a boundary piece covers parts of four map cells. The
//  caller must therefore draw the whole view one time for each Layer, in the
//  order of the layers. Inside one layer, the order of the cells along a row
//  is correct.

TORUS_GRID :: 4
CLASS_CAP :: 24
NETWORK_CAP :: 4
VARIANT_CAP :: 8

// The corner bits of a boundary case. The name of a bit says where its corner
// is in the dual cell, which covers parts of four map cells.
CORNER_NW :: 1 << 0
CORNER_NE :: 1 << 1
CORNER_SW :: 1 << 2
CORNER_SE :: 1 << 3

// class_set and the push_* calls fill this structure. Read the result of
// cell, and do not read these members.
Class :: struct {
	rank: u8,             // the order at a border. A higher rank covers a lower one.
	overlay_density: int, // the percent of the inner cells that carry an overlay
	ground_ids: [TORUS_GRID][TORUS_GRID]u32,      // [gy][gx], the windows of the offset grid
	overlay_ids: [dynamic;VARIANT_CAP]u32,        // the art inside a region
	edge_overlay_ids: [dynamic;VARIANT_CAP]u32,   // the art at the border of a region
}

// A config of all zeros is valid and empty (ZII). Write the seed directly. The
// calls below supply each other value. class_count is the largest class id
// that a call named. The module ignores an id at CLASS_CAP or above, so no
// caller can make a mistake here, and no caller must set a size first.
Config :: struct {
	seed: u64, // The module adds this to the hash of a position, so one world
	           // can take a different style.
	class_count: u32,
	classes: [CLASS_CAP]Class,
	mask_ids: [16][dynamic;VARIANT_CAP]u32, // by case code. One set serves each class.
	network_ids: [NETWORK_CAP][16][dynamic;VARIANT_CAP]u32, // by connection case
}

////////////////////////////////
//~ fp: Registration

@(private)
class_of :: proc(config: ^Config, klass: u32) -> ^Class {
	result: ^Class
	if klass < CLASS_CAP {
		config.class_count = max(config.class_count, klass + 1)
		result = &config.classes[klass]
	}
	return result
}

class_set :: proc(config: ^Config, klass: u32, rank: u8, overlay_density: int) {
	c := class_of(config, klass)
	if c != nil {
		c.rank = rank
		c.overlay_density = overlay_density
	}
}

push_ground :: proc(config: ^Config, klass: u32, gx, gy: int, id: u32) {
	c := class_of(config, klass)
	if c != nil && 0 <= gx && gx < TORUS_GRID && 0 <= gy && gy < TORUS_GRID {
		c.ground_ids[gy][gx] = id
	}
}

push_overlay :: proc(config: ^Config, klass: u32, id: u32) {
	c := class_of(config, klass)
	if c != nil && len(c.overlay_ids) < VARIANT_CAP {
		append(&c.overlay_ids, id)
	}
}

push_edge_overlay :: proc(config: ^Config, klass: u32, id: u32) {
	c := class_of(config, klass)
	if c != nil && len(c.edge_overlay_ids) < VARIANT_CAP {
		append(&c.edge_overlay_ids, id)
	}
}

push_mask :: proc(config: ^Config, code: int, id: u32) {
	if 0 <= code && code < 16 && len(config.mask_ids[code]) < VARIANT_CAP {
		append(&config.mask_ids[code], id)
	}
}

push_network :: proc(config: ^Config, network: int, code: int, id: u32) {
	if 0 <= network && network < NETWORK_CAP && 0 <= code && code < 16 &&
	   len(config.network_ids[network][code]) < VARIANT_CAP {
		append(&config.network_ids[network][code], id)
	}
}

////////////////////////////////
//~ fp: Cell Query

Layer :: enum {
	Surface, // the ground and the boundary shapes. Draw this layer first.
	Network, // the rivers and the roads: above the ground, below the objects
	Object,  // the overlays, which the map draws above each surface
}

Piece :: struct {
	id: u32,      // an art id that a call registered. 0 asks the caller for its own art.
	mask_id: u32, // the alpha mask to draw `id` through. 0 means no mask.
	klass: u32,   // the class of the piece, for a color and for a diagnostic
	layer: Layer,
	offset: geo.V2, // the offset from the origin of the cell, in cell units
}

Cell :: struct {
	pieces: [8]Piece, // the order to draw inside one cell. Across the cells,
	                  // draw one layer at a time.
	count: int,
}

// A noise value from a position. It is a hash of the seed, the position and
// the stream, and nothing more. The appearance therefore does not change from
// one frame to the next.
//
// `stream` separates the random choices that the module makes at one position.
// Stream 0 chooses an overlay. Stream 1 decides whether a cell has an overlay.
// Streams 2 to 2+CLASS_CAP choose a mask variant, one stream for each class.
// The streams of the networks come after those.
@(private)
noise :: proc(seed: u64, p: geo.V2i, stream: u32) -> u32 {
	return rng.hash_2d_stream(seed, p.x, p.y, stream)
}

// The order in which the classes cover each other. The rank decides. Two equal
// ranks compare their class ids.
@(private)
order :: proc(config: ^Config, klass: u32) -> u32 {
	return u32(config.classes[klass].rank) << 16 | klass
}

// `neighborhood` is the 3x3 window of classes around the cell, row by row.
// Index 4 is the cell itself. A class id at class_count or above reads as
// class 0, which is the diagnostic for a wrong id.
//
// `networks` is the connection mask of the cell for each network id. A value
// of 0 means no network (ZII).
//
// `p` is the position of the cell in the world.
cell :: proc(config: ^Config, neighborhood: [9]u32, networks: [NETWORK_CAP]u8, p: geo.V2i) -> Cell {
	result: Cell

	// A wrong id reads as class 0, which the caller draws in a bright color.
	nb: [9]u32
	for i in 0 ..< 9 {
		nb[i] = neighborhood[i] < config.class_count ? neighborhood[i] : 0
	}
	klass := nb[4]
	def := &config.classes[klass]

	//- fp: the stack of ground at the north west dual cell, whose corners are
	//  the cells NW, N, W and this one. The class at the bottom draws in full.
	//  Each class above it draws through the mask of its case, in the order
	//  that the classes cover each other.
	//
	//  The module cuts a window on the offset grid, so [(p-1)&3] is the part of
	//  the torus under this dual cell. A cell with one class and a cell with
	//  more both continue one texture for each class, with no seam.
	{
		corners := [4]u32{nb[0], nb[1], nb[3], nb[4]} // CORNER_* bit order
		wx := (p.x - 1) & (TORUS_GRID - 1)
		wy := (p.y - 1) & (TORUS_GRID - 1)
		present: [4]u32
		present_count := 0
		for i in 0 ..< 4 {
			seen := false
			for j in 0 ..< present_count { seen |= present[j] == corners[i] }
			if !seen {
				present[present_count] = corners[i]
				present_count += 1
			}
		}
		// in the order that the classes cover each other, from the bottom
		for i := 1; i < present_count; i += 1 {
			for j := i; j > 0 && order(config, present[j]) < order(config, present[j - 1]); j -= 1 {
				present[j], present[j - 1] = present[j - 1], present[j]
			}
		}
		// the class at the bottom, which draws in full, as if there were no border
		{
			bottom := present[0]
			piece := &result.pieces[result.count]
			result.count += 1
			piece.id = config.classes[bottom].ground_ids[wy][wx]
			piece.klass = bottom
			piece.layer = .Surface
			piece.offset = {-0.5, -0.5}
		}
		for i := 1; i < present_count; i += 1 {
			layer_klass := present[i]
			layer_def := &config.classes[layer_klass]
			code := 0
			for c in 0 ..< 4 {
				if order(config, corners[c]) >= order(config, layer_klass) { code |= 1 << uint(c) }
			}
			masks := config.mask_ids[code][:]
			if len(masks) == 0 { continue }
			ground_id := layer_def.ground_ids[wy][wx]
			if ground_id == 0 { continue } // no ground, nothing to spill
			piece := &result.pieces[result.count]
			result.count += 1
			piece.id = ground_id
			piece.mask_id = masks[int(noise(config.seed, p, 2 + layer_klass)) % len(masks)]
			piece.klass = layer_klass
			piece.layer = .Surface
			piece.offset = {-0.5, -0.5}
		}
	}

	//- fp: the networks, in the order of their ids. A cell with any network
	//  draws no overlay.
	networked := false
	for n in 0 ..< NETWORK_CAP {
		code := int(networks[n])
		if code == 0 { continue }
		networked = true
		ids := config.network_ids[n][code][:]
		if len(ids) == 0 { continue }
		piece := &result.pieces[result.count]
		result.count += 1
		piece.id = ids[int(noise(config.seed, p, u32(2 + CLASS_CAP + n))) % len(ids)]
		piece.klass = klass
		piece.layer = .Network
	}

	//- fp: the overlay art becomes thin at the border of a region. A cell at
	//  the border takes the sparse edge art, and a cell inside takes the full
	//  art. Some cells take no art. A group of mountains or a forest therefore
	//  reads as separate shapes, and not as one piece of art at every cell.
	if !networked {
		on_border := false
		for i in 0 ..< 9 { on_border |= nb[i] != klass }
		ids: []u32
		if on_border && len(def.edge_overlay_ids) > 0 {
			ids = def.edge_overlay_ids[:]
		} else if len(def.overlay_ids) > 0 &&
		   int(noise(config.seed, p, 1) % 100) < def.overlay_density {
			ids = def.overlay_ids[:]
		}
		if len(ids) > 0 {
			piece := &result.pieces[result.count]
			result.count += 1
			piece.id = ids[int(noise(config.seed, p, 0)) % len(ids)]
			piece.klass = klass
			piece.layer = .Object
		}
	}

	return result
}

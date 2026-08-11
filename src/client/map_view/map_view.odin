package map_view

import "core:fmt"
import "core:mem"
import "core:mem/virtual"

import d "../../draw"
import "../../chunk"
import "../../game"
import "../../game/defs"
import "../../game/tiling"
import "../../game/worldgen"
import "../../geo"
import "../../rng"
import "../../ui"
import "../../window"

////////////////////////////////
//~ fp: Map
//
// The view of the world of the client. The world itself belongs to the game
// layer. data/world.tabula holds the parameters of the generator, and
// data/terrain_types.tabula holds the terrain rows.
//
// One object holds the sprites, the registration of those sprites in the
// tiler, and the cache of the cells of the tiler. A caller does not read the
// members of that object.
//
// This module draws and does nothing more. It reads no input, and the camera
// arrives as a value.
//
// The C build called this module `map`, which Odin reserves for its map type,
// so the package is map_view.

TILE :: f32(8.0) // the units of the world for each tile

@(private) SPRITE_CAP :: 1024
@(private) PAWN_VARIANT_CAP :: 8

View :: struct {
	//- The assets: one table of sprites, and the registration in the tiler
	//  that the loader makes while it fills that table.
	sprites: [dynamic;SPRITE_CAP]d.Sprite, // The id 0 says that there is no art.
	tiler: tiling.Config,
	// The art of a pawn. Each game.Sprite gives a list of sprite ids, one for
	// each variant. An empty list says that the sprite has no art.
	gm_sprites: [game.Sprite][dynamic;PAWN_VARIANT_CAP]u32,

	//- The cache of the cells of the tiler. An entry that exists comes back as
	//  it is. Where an entry is absent, this module calls the tiler and keeps
	//  the answer. tiling.cell always gives one piece or more, so a cell of
	//  zeros says that the cache has no entry.
	cell_arena: virtual.Arena, // the memory of the cells. Each new world clears it.
	cells: []tiling.Cell,      // (width+1) by (height+1). The extra ring holds the dual cells of the far edges.
	cache_w: int,
	cache_h: int,
}

////////////////////////////////
//~ fp: Map Assets
//
// The loader tells the tiler what art exists, with the tiling.push_ functions.
// At the draw, tiling.cell answers with those same sprite ids, so the draw is
// a read of a table and holds no rule.
//
// The assets stay separate: a painting of the ground, a shape of a mask, and a
// piece of overlay art. No file holds a combination of two of those.

@(private)
register :: proc(view: ^View, sprite: d.Sprite) -> u32 {
	result: u32 = 0
	if len(view.sprites) < SPRITE_CAP {
		result = u32(len(view.sprites))
		append(&view.sprites, sprite)
	}
	return result
}

// It reads one png of a tile and gives a sprite id. A result of 0 says that
// the file does not exist, which also tells the loader to stop its search for
// more variants.
@(private)
load :: proc(view: ^View, path: string) -> u32 {
	result: u32 = 0
	image := d.image_load(path, context.temp_allocator)
	if image.w != 0 {
		result = register(view, d.spritesheet_push(image))
	}
	return result
}

// Set the size of the cache of the cells to the size of a new world. The
// cells go on an arena that the view owns, and this call clears it.
world_changed :: proc(view: ^View, board_w, board_h: int) {
	allocator := virtual.arena_allocator(&view.cell_arena)
	free_all(allocator)
	view.cache_w = board_w + 1
	view.cache_h = board_h + 1
	view.cells = make([]tiling.Cell, view.cache_w * view.cache_h, allocator)
}

@(private)
cell_of :: proc(view: ^View, ground: ^game.Map_Ground) -> ^tiling.Cell {
	cell := &view.cells[ground.pos.y * view.cache_w + ground.pos.x]
	if cell.count == 0 {
		nb: [9]u32
		for i in 0 ..< 9 { nb[i] = u32(ground.neighbours[i]) }
		networks: [tiling.NETWORK_CAP]u8
		for feature in defs.Feature {
			networks[int(feature)] = ground.features[feature]
		}
		cell^ = tiling.cell(&view.tiler, nb, networks, ground.pos)
	}
	return cell
}

// It loads the assets. A caller must call worldgen.terrain_table_load first.
init :: proc(allocator: mem.Allocator) -> ^View {
	view := new(View, allocator)
	if virtual.arena_init_growing(&view.cell_arena) != nil { panic("out of memory: cell arena") }
	append(&view.sprites, d.Sprite{}) // the id 0 is the nil sprite
	scratch := context.temp_allocator
	d.spritesheet_begin(512, 512, .Smooth)

	for type in 0 ..< len(worldgen.TERRAIN_TYPES) {
		def := &worldgen.TERRAIN_TYPES[type]
		name := worldgen.terrain_name(type)
		tiling.class_set(&view.tiler, u32(type), def.rank, def.overlay_density)

		// The ground. Each terrain has one painting that wraps on both axes.
		// The loader cuts that painting into 4x4 windows on the OFFSET grid,
		// which is half a tile past the grid of the painting itself, because
		// the tiler draws the ground on the dual cells. See the tiling package.
		//
		// The margin of the wrap keeps each window of that offset grid one
		// continuous part of the image. push_window fills the gutter of each
		// window in the sheet with the true texels next to that window, so two
		// windows meet with no seam on the screen.
		ground_path := fmt.tprintf("assets/tiles/%s_ground.png", name)
		torus := d.image_load(ground_path, scratch)
		if torus.w > 0 {
			tw := torus.w / tiling.TORUS_GRID
			th := torus.h / tiling.TORUS_GRID
			ext := d.image_create(torus.w + tw, torus.h + th, {}, scratch)
			d.image_blit(ext, 0, 0, torus)
			d.image_blit(ext, torus.w, 0, torus) // the margins of the wrap. The blit clips what goes past the image.
			d.image_blit(ext, 0, torus.h, torus)
			d.image_blit(ext, torus.w, torus.h, torus)
			for gy in 0 ..< tiling.TORUS_GRID {
				for gx in 0 ..< tiling.TORUS_GRID {
					wx0 := gx * tw + tw / 2
					wy0 := gy * th + th / 2
					win := geo.Rect{{f32(wx0), f32(wy0)}, {f32(wx0 + tw), f32(wy0 + th)}}
					tiling.push_ground(&view.tiler, u32(type), gx, gy,
					                   register(view, d.spritesheet_push_window(ext, win)))
				}
			}
		}

		for variant := 0;; variant += 1 {
			path := fmt.tprintf("assets/tiles/%s_overlay_%d.png", name, variant)
			id := load(view, path)
			if id == 0 { break }
			tiling.push_overlay(&view.tiler, u32(type), id)
		}
		for variant := 0;; variant += 1 {
			path := fmt.tprintf("assets/tiles/%s_edge_%d.png", name, variant)
			id := load(view, path)
			if id == 0 { break }
			tiling.push_edge_overlay(&view.tiler, u32(type), id)
		}
	}

	// A mask of a boundary does not name a class of terrain. The cases are 1
	// to 14. The case 0 is an empty cell and the case 15 is a full cell, and
	// neither has a file.
	for code in 1 ..< 15 {
		for variant := 0;; variant += 1 {
			path := fmt.tprintf("assets/tiles/mask_%d_%d.png", code, variant)
			id := load(view, path)
			if id == 0 { break }
			tiling.push_mask(&view.tiler, code, id)
		}
	}

	// The art of a network, at the id of each defs.Feature. There is one
	// complete piece for each case of the connections. The prefix of a file is
	// the name of the feature, so this loop reads each feature that exists and
	// names none of them.
	feature_names := game.FEATURE_NAMES
	for feature in defs.Feature {
		for code in 1 ..< 16 {
			for variant := 0;; variant += 1 {
				path := fmt.tprintf("assets/tiles/%s_%d_%d.png", feature_names[feature], code, variant)
				id := load(view, path)
				if id == 0 { break }
				tiling.push_network(&view.tiler, int(feature), code, id)
			}
		}
	}

	// The art of a pawn, at the id of each game.Sprite. The game layer says
	// what a thing looks like, and these files hold that appearance. This loop
	// searches for the variants, as it does for each other kind of tile art. A
	// sprite with no file keeps a count of 0, and the renderer then draws a
	// flat shape. The None sprite is the nil sprite, and it has no art.
	sprite_names := game.SPRITE_NAMES
	for sprite in game.Sprite {
		if sprite == .None { continue }
		for variant in 0 ..< PAWN_VARIANT_CAP {
			path := fmt.tprintf("assets/tiles/site_%s_%d.png", sprite_names[sprite], variant)
			id := load(view, path)
			if id == 0 { break }
			append(&view.gm_sprites[sprite], id)
		}
	}

	d.spritesheet_end()
	return view
}

////////////////////////////////
//~ fp: Drawing

// The rect in world space of the tile at (x, y). A position with a fraction is
// on the dual grid. The rect becomes smaller by `inset` units of the world at
// each side.
@(private)
tile_rect :: proc(x, y: f32, inset: f32) -> geo.Rect {
	return {{x * TILE + inset, y * TILE + inset},
	        {(x + 1) * TILE - inset, (y + 1) * TILE - inset}}
}

// The cell of the tile below a position on the screen. That position is in
// the points of the client area, as window.mouse_pos gives it.
//
// This function rounds toward the lower value, so a cell to the west of the
// board and a cell to the north of it come back with a value below 0. The
// caller must test the result against the size of the board.
tile_from_screen :: proc(camera: d.Camera, screen: geo.V2) -> geo.V2i {
	world := d.camera_from_screen(camera, screen)
	tile := geo.V2i{int(world.x / TILE), int(world.y / TILE)}
	// A cast to int rounds toward 0. The subtraction of the result of the
	// comparison changes that into a round toward the lower value, which is
	// what a coordinate below 0 needs.
	tile.x -= f32(tile.x) * TILE > world.x ? 1 : 0
	tile.y -= f32(tile.y) * TILE > world.y ? 1 : 0
	return tile
}

// The ground of one layer of the tiler, across the whole window.
//
// A boundary piece covers a part of each cell around its own cell, so a whole
// layer must go down before the next layer starts. See the tiling package.
// This is the one order that the lists of the items do not already give, so it
// is the one loop over a layer in this module.
//
// A piece takes no tint: a piece of the ground is on the dual grid, half a
// cell from the tile of its item, so a color of one tile cannot go on it.
@(private)
draw_ground :: proc(view: ^View, items: game.Map_Items, layer: tiling.Layer) {
	for &ground in items.ground {
		cell := cell_of(view, &ground)
		for pi in 0 ..< cell.count {
			piece := &cell.pieces[pi]
			if piece.layer != layer || piece.id == 0 { continue }
			r := tile_rect(f32(ground.pos.x) + piece.offset.x,
			               f32(ground.pos.y) + piece.offset.y, 0)
			if piece.mask_id != 0 {
				d.sprite_masked(view.sprites[piece.id], view.sprites[piece.mask_id], r, {})
			} else {
				d.sprite(view.sprites[piece.id], r, {})
			}
		}
	}
}

// The shapes above the ground, in the order of the list. A shape says what to
// draw and where, and says nothing about what it stands for, so this loop
// reads the art alone. It has three cases:
//
//   the nil sprite     the shape asks for no art, and the color goes across
//                      the whole tile
//   art that exists    the art across the whole tile, with the color of the
//                      shape as its tint, where white keeps the colors of the
//                      art
//   art that is absent a placeholder, because the loader found no file for the
//                      sprite that the shape names
@(private)
draw_shapes :: proc(view: ^View, items: game.Map_Items) {
	it := chunk.iterator(items.shapes)
	for shape in chunk.iterate(&it) {
		variant_count := len(view.gm_sprites[shape.sprite])
		if shape.sprite == .None {
			d.rect(tile_rect(f32(shape.pos.x), f32(shape.pos.y), 0), shape.color)
		} else if variant_count > 0 {
			// A hash of the id chooses the variant, so that variant stays equal
			// for the life of the thing, and two things take two variants.
			variant := int(rng.hash_u64(0, u64(shape.id)) % u64(variant_count))
			sprite := view.sprites[view.gm_sprites[shape.sprite][variant]]
			d.sprite(sprite, tile_rect(f32(shape.pos.x), f32(shape.pos.y), 0), shape.color)
		} else {
			inset := TILE * 0.2
			rounding := (TILE - 2 * inset) * 0.35
			d.rect_rounded(tile_rect(f32(shape.pos.x), f32(shape.pos.y), inset), shape.color, rounding)
		}
	}
}

// The ground first, because a shape stands above it. The shapes next, in the
// order of their list. The mark of the selection last, above everything.
draw :: proc(view: ^View, items: game.Map_Items, camera: d.Camera) {
	vp := window.size()
	d.rect({{0, 0}, vp}, {0.06, 0.06, 0.08, 1})

	d.camera_begin(camera)
	for layer in tiling.Layer {
		draw_ground(view, items, layer)
	}
	draw_shapes(view, items)
	if items.has_mark {
		d.rect_outline(tile_rect(f32(items.mark.x), f32(items.mark.y), 0),
		               ui.color_from_name("accent"), 1.0)
	}
	d.camera_end()
}

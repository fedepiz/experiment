package main

import "core:fmt"
import "core:math"
import "core:mem/virtual"
import "core:os"
import "core:strconv"

import "client"
import "client/hud"
import "client/map_view"
import "color"
import "draw"
import "game"
import "game/worldgen"
import "geo"
import "tabula"
import "ui"
import "window"

////////////////////////////////
//~ fp: temp: Test Render
//
// A scene that shows what the draw layer does. No code calls it now. Call it
// inside the frame, in place of map_view.draw or after it, to test the draw
// layer again. It owns its assets, and it loads them at its first call.

test_render :: proc(camera: draw.Camera) {
	@(static) initialized: bool
	@(static) font: draw.Font
	@(static) checker: draw.Sprite
	@(static) stripes: draw.Sprite
	@(static) stripes_angle: f32
	if !initialized {
		initialized = true
		font = draw.font_open("assets/fonts/Arial.ttf")
		SPRITE_DIM :: 128
		checker_img := draw.image_create(SPRITE_DIM, SPRITE_DIM, {}, context.temp_allocator)
		stripes_img := draw.image_create(SPRITE_DIM, SPRITE_DIM, {}, context.temp_allocator)
		for y in 0 ..< SPRITE_DIM {
			for x in 0 ..< SPRITE_DIM {
				c_on := ((x >> 4) + (y >> 4)) & 1 == 1
				draw.image_set_px(
					checker_img,
					x,
					y,
					c_on ? {0.90, 0.35, 0.78, 1} : {0.16, 0.16, 0.19, 1},
				)
				s_on := ((x + y) >> 4) & 1 == 1
				draw.image_set_px(
					stripes_img,
					x,
					y,
					s_on ? {0.24, 0.82, 0.75, 1} : {0.08, 0.24, 0.24, 1},
				)
			}
		}
		draw.spritesheet_begin(512, 256, .Smooth)
		checker = draw.spritesheet_push(checker_img)
		stripes = draw.spritesheet_push(stripes_img)
		draw.spritesheet_end() // the pixels are on the GPU now
	}

	vp := window.size()
	draw.rect_gradient_v({{0, 0}, vp}, {0.09, 0.09, 0.13, 1}, {0.03, 0.03, 0.05, 1})

	draw.rect_rounded({{100, 100}, {500, 300}}, {0.2, 0.4, 0.9, 1}, 24)
	draw.rect_outline({{100, 350}, {500, 550}}, {1, 1, 1, 1}, 4)

	draw.sprite(checker, {{550, 100}, {806, 356}}, {}) // a tint of 0 keeps the colors of the sprite
	stripes_angle += 0.5 * window.frame_time() // half a radian for each second
	sp: draw.Sprite_Params
	sp.sprite = stripes
	sp.dst = {{880, 120}, {1080, 320}}
	sp.rotation = stripes_angle
	draw.sprite_ex(sp)

	draw.push_clip({{1150, 120}, {1350, 320}})
	draw.rect({{1100, 100}, {1500, 340}}, {0.9, 0.4, 0.8, 1})
	draw.pop_clip()

	draw.line({550, 420}, {1050, 480}, 3, {1.0, 0.8, 0.2, 1})

	draw.camera_begin(camera)
	draw.rect_rounded({{-60, 120}, {60, 200}}, {0.2, 0.8, 0.4, 1}, 10)
	draw.camera_end()

	draw.text(font, 40, {100, 600}, color.WHITE, "Imperium — draw layer online")
	draw.text(
		font,
		18,
		{100, 660},
		color.rgb(0.7, 0.7, 0.75),
		"rects, sprites, sheets, clip, camera, text",
	)

	// hsva moves the hue, and with_alpha lowers the alpha.
	for i in 0 ..< 24 {
		t := f32(i) / 24.0
		draw.rect(
			{{100 + f32(i) * 30, 720}, {128 + f32(i) * 30, 760}},
			color.with_alpha(color.hsva(t, 0.85, 1.0, 1.0), 1.0 - t * 0.7),
		)
	}
}

////////////////////////////////
//~ fp: 60fps Present Pacing
//
// The hardware presents the frames at 60 for each second. Where the rate of
// the monitor is a whole multiple of 60, such as 60Hz or 120Hz, the window
// presents at each Nth vertical blank. The clock of the display then gives
// exactly 60 on each screen, and a drag between two monitors of different
// rates reads window.refresh_rate again.
//
// Where the rate is not such a multiple, such as 144Hz or 90Hz, the window
// presents at each vertical blank. The tolerance of 2Hz absorbs a rate that
// is not an integer, so 119.98 reads as 120.
//
// Call this function one time in each frame.

pace_60fps_update :: proc() {
	@(static) current_interval: int
	hz := window.refresh_rate()
	multiple := int(hz / 60.0 + 0.5)
	interval := 1
	if multiple >= 1 && 60.0 * f32(multiple) - 2.0 < hz && hz < 60.0 * f32(multiple) + 2.0 {
		interval = multiple
	}
	if interval != current_interval {
		current_interval = interval
		window.set_swap_interval(interval)
	}
}

////////////////////////////////
//~ fp: Camera
//
// The code that steers the camera is here, and not in client/map_view, because
// the map draws and does nothing more, and because the meaning of a key is a
// decision of this file. The camera itself is a draw.Camera value, which the
// map receives and does not change.

camera_inertial_move :: proc(camera: ^draw.Camera, translation: geo.V2, zooming: f32) {
	dt := window.frame_time()
	zoom := camera.zoom == 0 ? 1.0 : camera.zoom

	//- The movement with inertia. The keys set a target velocity, and the
	//  velocity of the camera moves toward that target along an exponential
	//  curve. That one curve gives the increase of speed at a press and the
	//  decrease to a stop at a release.
	//
	//  `blend` is the part of the difference that this frame removes. dt is in
	//  the exponent, so two frames of a half length give the same result as one
	//  frame of a full length, and the movement therefore feels the same at
	//  each frame rate. A larger `rate` makes the camera answer faster: the
	//  velocity falls to one half in 1/rate seconds.
	blend := 1.0 - math.pow(f32(2.0), -8.0 * dt)

	// The target of the pan is in units of the world for each second,
	// multiplied by 1/zoom. A pan therefore covers the same part of the screen
	// at each zoom.
	//
	// The target of the zoom is in doublings for each second. That measure is
	// exponential, so a zoom feels equally fast at each level. The factor of
	// 1/zoom belongs to the pan alone.
	pan_target := translation * (400.0 / zoom)
	zoom_target := 3.0 * zooming
	camera.pan_vel = camera.pan_vel + (pan_target - camera.pan_vel) * blend
	camera.zoom_vel += (zoom_target - camera.zoom_vel) * blend

	camera.center = camera.center + camera.pan_vel * dt
	zoom *= math.pow(f32(2.0), camera.zoom_vel * dt)
	camera.zoom = clamp(zoom, 0.25, 8.0)
}

////////////////////////////////
//~ fp: Entry Point

main :: proc() {
	//- fp: temp: The mode that writes a report stops the program before a
	//  window exists.
	args := os.args
	if len(args) >= 3 && args[1] == "worlds" {
		world_count, count_ok := strconv.parse_u64(args[2])
		first_seed: u64 = 1
		seed_ok := true
		if len(args) >= 4 {first_seed, seed_ok = strconv.parse_u64(args[3])}
		if !count_ok || !seed_ok {
			fmt.eprintfln("usage: %s worlds N [first_seed]", args[0])
			os.exit(1)
		}
		report_worldgen(int(world_count), first_seed)
		return
	}

	//- The arena of one frame. The loop frees it after each swap, as the C
	//  program clears its frame arena. The arenas below it live for the run of
	//  the program.
	frame_arena: virtual.Arena
	if virtual.arena_init_growing(&frame_arena) != nil { panic("out of memory: frame arena") }
	frame_allocator := virtual.arena_allocator(&frame_arena)
	persist_arena: virtual.Arena
	if virtual.arena_init_growing(&persist_arena) != nil { panic("out of memory: persist arena") }
	persist_allocator := virtual.arena_allocator(&persist_arena)

	window.open("Imperium", 1600, 900)
	window.equip_gl()
	draw.init()
	ui.init(hud.measure_text, hud.font_metrics, nil)
	ui.set_theme(ui.theme_load("data/ui.tabula", persist_allocator))

	//- fp: The state of the game, which exists for the run of the program and
	//  owns its arena. The view of the map follows it: a new seed builds the
	//  cache of the cells of that view again.
	g: game.Game
	game_next_seed: u64 = 2704

	// The terrain table gives the name of each asset and the registration in
	// the tiler.
	worldgen.terrain_table_load("data/terrain_types.tabula")
	view := map_view.init(persist_allocator) // it stays across each new seed

	hud_font := draw.font_open("assets/fonts/Arial.ttf")
	fps_counter: client.FPS_Counter

	camera: draw.Camera
	hud_mouse_over := false // as of the last frame that the code built

	map_mode_flags: bit_set[game.Map_Mode_Flag] = {.Pawns}

	// The frame, in phases: read the input, then handle the keys and the
	// clicks, then set the rate, then initialize the world where that is
	// necessary, then run the simulation, then draw the map and the HUD, then
	// move the camera, then swap.
	//
	// camera_inertial_move runs AFTER the draw, and it does so deliberately.
	// The frame therefore draws with the camera value that the code for the
	// clicks read, which is one frame old. hud_mouse_over follows that same
	// rule.
	//
	// A request to close finishes its frame. The test of the loop reads that
	// request at the start of the next frame.
	for keep_going := true; keep_going && !window.close_requested(); {
		window.poll()

		cmd: client.Command
		client.cmd_from_keyboard(&cmd)

		hud_list: ui.Draw_List
		{
			info := game.info(&g, frame_allocator)

			{
				// add the frames for each second
				text := fmt.aprintf("%.1f", fps_counter.display, allocator = frame_allocator)
				tabula.add_string(info, "fps", text, frame_allocator)
			}

			client.fps_update(&fps_counter)
			ui.frame_begin(frame_allocator, hud.gather_input())
			hud.build(hud_font, info, &cmd)
			hud_list = ui.frame_end()
		}

		if cmd.quit {keep_going = false}

		if cmd.reload {g.initialised = false}

		if cmd.toggle_pause {g.paused = !g.paused}

		if g.initialised && !hud_mouse_over {
			if window.mouse_pressed(.Left) {
				game.select(&g, map_view.tile_from_screen(camera, window.mouse_pos()))
			}
			if window.mouse_pressed(.Right) {
				game.deselect(&g)
			}
		}

		map_mode_flags ~= cmd.map_mode_toggle

		pace_60fps_update()

		if !g.initialised {
			// The terrain table reloads with the world, so a hot edit of the
			// file shows on the next reload. The assets of the map keep the rows
			// of the read at map_view.init.
			worldgen.terrain_table_load("data/terrain_types.tabula")
			game.init(&g, game_next_seed)
			game_next_seed += 1
			map_view.world_changed(view, g.board.width, g.board.height)
			// set the camera to its first state
			camera.center = {
				f32(g.board.width) * map_view.TILE / 2,
				f32(g.board.height) * map_view.TILE / 2,
			}
			camera.zoom = 1.0 // the whole map is in view. The wheel makes the view smaller.
		}

		game.update(&g, window.frame_time())

		draw.frame_begin(frame_allocator, window.size_px(), window.scale())
		{
			mode: game.Map_Mode
			// The window of the tiles that a person sees. It holds the cells
			// below the corners of the screen, and one ring of cells around
			// them.
			//
			// The far edge takes one MORE ring. Each cell owns the dual cell of
			// the boundary at its corner to the north west. See the tiling
			// package. The dual cells past the last column and the last row of
			// the board therefore need an owner, and the +1 at the end reaches
			// the ring cells that game.map_items makes for that purpose.
			board_w := g.board.width
			board_h := g.board.height
			tile_min := map_view.tile_from_screen(camera, {0, 0})
			tile_max := map_view.tile_from_screen(camera, window.size())
			mode.window = {
				{max(tile_min.x - 1, 0), max(tile_min.y - 1, 0)},
				{min(tile_max.x + 1, board_w - 1) + 1, min(tile_max.y + 1, board_h - 1) + 1},
			}

			mode.flags = map_mode_flags
			items := game.map_items(&g, mode, frame_allocator)
			map_view.draw(view, items, camera)
			// test_render(camera) // fp: the scene that shows what the draw layer does

			hud.replay_draw_list(hud_list)
			hud_mouse_over = hud_list.mouse_over_ui
		}
		draw.frame_end()

		camera_inertial_move(&camera, cmd.translation, cmd.zooming) // It runs after the draw. See the comment on the phases above the loop.

		window.swap() // The vertical sync makes the loop run at the rate of the display.
		free_all(frame_allocator)
		free_all(context.temp_allocator)
	}
	window.close()
}

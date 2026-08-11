package client

import "../game"
import "../geo"
import "../window"

////////////////////////////////
//~ fp: Client
//
// The commands of a frame, read from the keyboard. The window layer answers
// the state of each key, and this module gives that state a meaning.

Command :: struct {
	reload:          bool,
	quit:            bool,
	translation:     geo.V2,
	zooming:         f32,
	map_mode_toggle: bit_set[game.Map_Mode_Flag],
	toggle_pause:    bool,
}

cmd_from_keyboard :: proc(cmd: ^Command) {
	INTERACTIONS :: [?]struct {
		key:     window.Key,
		x, y, z: f32,
	}{{.A, -1, 0, 0}, {.D, 1, 0, 0}, {.W, 0, -1, 0}, {.S, 0, 1, 0}, {.Q, 0, 0, 1}, {.E, 0, 0, -1}}

	d_trans: geo.V2
	d_zoom: f32 = 0

	for interaction in INTERACTIONS {
		if window.key_down(interaction.key) {
			d_trans.x += interaction.x
			d_trans.y += interaction.y
			d_zoom += interaction.z
		}
	}

	cmd.translation = cmd.translation + d_trans
	cmd.zooming += d_zoom
	cmd.reload = window.key_pressed(.R)
	cmd.quit = window.key_pressed(.Escape)

	cmd.toggle_pause |= window.key_pressed(.Space)
	if window.key_pressed(.Num1) {
		cmd.map_mode_toggle |= {.Influence}
	}
}

////////////////////////////////
//~ fp: FPS Counter
//
// This counter reads window.frame_time. Call the update one time in each
// frame, and read the value at any time. The value changes four times in each
// second. The value of 1/dt of one frame changes too fast for a person to
// read.

FPS_Counter :: struct {
	accum_time:   f32,
	accum_frames: int,
	display:      f32, // the frames for each second, as of the last change of this value
}

fps_update :: proc(counter: ^FPS_Counter) {
	counter.accum_time += window.frame_time()
	counter.accum_frames += 1
	if counter.accum_time >= 0.25 {
		counter.display = f32(counter.accum_frames) / counter.accum_time
		counter.accum_time = 0
		counter.accum_frames = 0
	}
}

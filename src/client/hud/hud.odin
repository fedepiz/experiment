package hud

import "core:fmt"

import "../"
import "../../chunk"
import "../../draw"
import "../../geo"
import "../../tabula"
import "../../ui"
import "../../window"

////////////////////////////////
//~ fp: HUD
//
// This module joins the UI core, which uses the base layer alone, to the gfx
// layer. The draw layer answers the two functions that measure a text. This
// module reads the input into a ui.Input, and draws the list of commands of
// the frame with the draw functions. A font crosses that boundary as the u64
// of a draw.Font.
//
// build makes the panel of the selection, a bar of actions that stands in
// place of the true one, and the counter of the frames.

//- fp: the two functions for ui.init, and the calls of each frame

measure_text :: proc(user: rawptr, font: u64, size: f32, text: string) -> geo.V2 {
	return draw.text_dim(draw.Font(font), size, text)
}

font_metrics :: proc(user: rawptr, font: u64, size: f32) -> ui.Font_Metrics {
	m := draw.font_metrics(draw.Font(font), size)
	return {m.ascent, m.descent, m.line_advance}
}

// The position of the mouse is in the points of the client area, which is the
// unit of the UI.
gather_input :: proc() -> ui.Input {
	input: ui.Input
	input.mouse = window.mouse_pos()
	input.down[.Left] = window.mouse_down(.Left)
	input.down[.Right] = window.mouse_down(.Right)
	input.down[.Middle] = window.mouse_down(.Middle)
	input.pressed[.Left] = window.mouse_pressed(.Left)
	input.pressed[.Right] = window.mouse_pressed(.Right)
	input.pressed[.Middle] = window.mouse_pressed(.Middle)
	input.released[.Left] = window.mouse_released(.Left)
	input.released[.Right] = window.mouse_released(.Right)
	input.released[.Middle] = window.mouse_released(.Middle)
	input.scroll = window.scroll()
	input.dt = window.frame_time()
	input.window = window.size()
	return input
}

replay_draw_list :: proc(list: ui.Draw_List) {
	it := chunk.iterator(list.commands)
	for cmd in chunk.iterate(&it) {
		switch cmd.kind {
		case .Rect:
			params: draw.Rect_Params
			params.rect = cmd.rect
			params.colors = cmd.colors
			params.corner_radii = cmd.corner_radii
			params.border_thickness = cmd.border_thickness
			params.edge_softness = cmd.edge_softness
			draw.rect_ex(params)
		case .Text:
			draw.text(draw.Font(cmd.font), cmd.font_size, cmd.pos, cmd.color, cmd.text)
		case .Clip_Push:
			draw.push_clip(cmd.rect)
		case .Clip_Pop:
			draw.pop_clip()
		case .None:
		}
	}
}

////////////////////////////////
//~ fp: The Panels

// One line of a label and a value. The label is at the left, in the color of
// the text, and the value is at the right. A row with a hover string shows
// that string in a tooltip while the mouse rests on the row.
@(private)
stat_row :: proc(label, value: string, value_color: geo.V4, hover: string) {
	row := ui.row_begin(label)
	row.pref_size[.X] = ui.size_pct(1, 0)
	ui.label(label)
	ui.spacer(ui.size_grow(1))
	ui.push_text_color(value_color)
	ui.label(value)
	ui.pop_text_color()
	ui.row_end()
	if len(hover) > 0 {
		row.flags += {.Clickable}
		if ui.signal(row).hovered {
			ui.tooltip(hover)
		}
	}
}

// One typed row of the facts. The game names the form of the row in `type`:
// `text` and `number` show `value`, `balance` shows `value` with its sign, and
// `x_of_y` shows "value of limit". A change of a tick belongs in the hover of
// the row, and never in the line.
@(private)
typed_row :: proc(key: string, row: ^tabula.Value) {
	type := tabula.get_string(row, "type")
	name := tabula.get_string(row, "name") or_else key
	hover := tabula.get_string(row, "hover")
	text: string
	color := ui.color_from_name("accent")
	switch type {
	case "text":
		text = tabula.get_string(row, "value")
		color = ui.color_from_name("text")
	case "number":
		text = tabula.get(row, "value").text
	case "balance":
		text = fmt.tprintf("%+g", tabula.get_num(row, "value"))
	case "x_of_y":
		text = fmt.tprintf("%g of %g", tabula.get_num(row, "value"), tabula.get_num(row, "limit"))
	}
	stat_row(name, text, color, hover)
}

// The part of the panel of the selection that reads each member of the tree
// of facts. A new member therefore shows with no change to this file.
//
// An object that holds a `type` becomes one typed row. An object without a
// `type` becomes a header of a section, in a color of a low contrast, and the
// members of that object become the rows below that header. A plain string or
// number becomes a simple row.
//
// `omit` names one key of the top level that this function does not show,
// which is the key of the title above and the `name` of a section.
//
// Each string is on the allocator of the caller, for the time of the build.
@(private)
fact_rows :: proc(object: ^tabula.Value, omit: string) {
	for node := object.first_member; node != nil; node = node.next {
		if len(omit) > 0 && node.key == omit {continue}
		value := &node.value
		#partial switch value.kind {
		case .Identifier, .String:
			stat_row(node.key, value.text, ui.color_from_name("text"), "")
		case .Number:
			stat_row(node.key, value.text, ui.color_from_name("accent"), "")
		case .Object:
			if !tabula.value_is_nil(tabula.get(value, "type")) {
				typed_row(node.key, value)
			} else {
				ui.spacer(ui.size_points(2, 1))
				ui.push_text_color(ui.color_from_name("muted"))
				ui.label(tabula.get_string(value, "name") or_else node.key)
				ui.pop_text_color()
				fact_rows(value, "name")
			}
		}
	}
}

// The panel at the top left corner, which shows the current selection. It
// holds a title, which is the name of a thing or the terrain of a tile, and
// then the rows of the facts.
@(private)
selection_panel :: proc(info: ^tabula.Value) {
	if tabula.value_is_nil(info) {return}

	{
		panel := ui.panel("selection_panel")
		panel.padding = {12, 10}
		panel.flags += {.Floating}
		panel.floating_anchor = {0, 0}
		panel.floating_pos = {12, 12}
		panel.pref_size[.X] = ui.size_points(240, 1)

		title, has_title := tabula.get_string(info, "name")
		if !has_title {
			terrain := tabula.get(tabula.get(info, "tile"), "terrain")
			title = tabula.get_string(terrain, "value") or_else "tile"
		}
		ui.push_font_size(19)
		ui.label(title)
		ui.pop_font_size()
		fact_rows(info, "name")
	}

	actions := tabula.get(info, "actions")

	if !tabula.value_is_nil(actions) {
		panel := ui.panel("action_panel")
		panel.padding = {12, 10}
		panel.flags += {.Floating}
		panel.floating_anchor = {0.0, 0.45}
		panel.floating_pos = {12, 12}
		panel.pref_size[.X] = ui.size_points(240, 1)

		ui.push_font_size(19)
		ui.label("Actions")
		ui.pop_font_size()

		it := tabula.iterator(actions)
		ui.push_pref_width({kind = .Points, value = 100, strictness = 1})
		for key, value in tabula.iterate(&it) {
			switch (key) {
			case "label":
				ui.label(value.text)
			case "action":
				name := tabula.get_string(value, "name") or_else "???"
				if ui.button(name).clicked {
					fmt.printf("Button %v clicked\n", name)
				}

			}
		}
		ui.pop_pref_width()
	}

}

// A bar along the bottom edge, at the width of the window. Each button of that
// bar stands in place of a true one. The bar is a strip and not a card: its
// height does not change, and it has a space at its left and at its right
// alone.
@(private)
action_bar :: proc(cmd: ^client.Command) {
	bar := ui.panel("action_bar")
	bar.padding = {10, 0}
	bar.flags += {.Floating}
	bar.floating_anchor = {0, 1}
	bar.pref_size[.X] = ui.size_pct(1, 1)
	bar.pref_size[.Y] = ui.size_points(40, 1)
	ui.push_pref_height(ui.size_grow(1))
	ui.push_child_align(.Center)
	{
		ui.row("actions")
		if ui.button("Toggle Influence").clicked {
			cmd.map_mode_toggle |= {.Influence}
		}
	}
	ui.pop_child_align()
	ui.pop_pref_height()
}

// The top right corner, which shows the value that fps_update writes.
@(private)
fps_panel :: proc(info: ^tabula.Value) {
	panel := ui.panel("fps_panel")
	panel.padding = {8, 4}
	panel.flags += {.Floating}
	panel.floating_anchor = {1, 0}
	panel.floating_pos = {-12, 12}
	text := tabula.get_string(info, "fps") or_else "NO FPS"
	ui.labelf("FPS: %s", text)
}

build :: proc(font: draw.Font, info: ^tabula.Value, cmd: ^client.Command) {
	ui.push_font(u64(font))
	ui.push_font_size(15)
	ui.push_child_gap(8)

	selection := tabula.get(info, "selection")
	selection_panel(selection)
	action_bar(cmd)
	fps_panel(info)

	ui.pop_child_gap()
	ui.pop_font_size()
	ui.pop_font()
}


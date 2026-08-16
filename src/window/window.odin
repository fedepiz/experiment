package window

import "base:runtime"
import "core:strings"
import "core:time"
import gl "vendor:OpenGL"
import "vendor:glfw"

import "../geo"

////////////////////////////////
//~ fp: Window
//
// This layer holds one window of the operating system: the open and the close,
// the input events of that window, and the GL context that draws into it.
// GLFW is the one backend, on each platform.
//
// Each coordinate is in the points of the client area, from the top left
// corner, with y that grows toward the bottom.
//
// size_px gives the framebuffer in pixels, and scale gives the pixels for each
// point, which is 2 on a screen of a high density. The renderer needs those
// two values to set up its framebuffer. No other code needs them.

@(private)
window: glfw.WindowHandle

////////////////////////////////
//~ fp: Scale Override
//
// The platform speaks in its own screen units. On macOS those units are
// points, and the content scale of GLFW says how many pixels each one holds.
// On X11 those units are pixels, and a session that sets no Xft.dpi reports a
// content scale of 1 on every screen, which makes each point one pixel and
// the window small on a screen of a high density.
//
// The override says: one point is `s` screen units. Each function of this
// layer converts at its own border, so the rest of the program sees points
// everywhere. A value of 0 removes the override, and the platform value
// stands (ZII).

@(private) forced_scale: f32

// Call it before open, so the window opens at the size in points that open
// receives.
set_scale :: proc(s: f32) {
	forced_scale = s
}

// the screen units of the platform for each point
@(private)
screen_per_point :: proc() -> f32 {
	return (forced_scale > 0) ? forced_scale : 1.0
}

// w and h are in points
open :: proc(title: string, w, h: int) {
	ok := glfw.Init()
	assert(bool(ok))
	s := screen_per_point()
	w := int(f32(w) * s)
	h := int(f32(h) * s)

	// The renderer writes its shaders at #version 410 core, and macOS gives a
	// core profile only with the forward compatibility flag.
	glfw.WindowHint(glfw.CONTEXT_VERSION_MAJOR, 4)
	glfw.WindowHint(glfw.CONTEXT_VERSION_MINOR, 1)
	glfw.WindowHint(glfw.OPENGL_PROFILE, glfw.OPENGL_CORE_PROFILE)
	glfw.WindowHint(glfw.OPENGL_FORWARD_COMPAT, glfw.TRUE)

	title_c := strings.clone_to_cstring(title, context.temp_allocator)
	window = glfw.CreateWindow(i32(w), i32(h), title_c, nil, nil)
	assert(window != nil)

	glfw.SetKeyCallback(window, key_callback)
	glfw.SetMouseButtonCallback(window, mouse_button_callback)
	glfw.SetCursorPosCallback(window, cursor_pos_callback)
	glfw.SetScrollCallback(window, scroll_callback)
	glfw.SetWindowSizeCallback(window, size_callback)
	glfw.SetWindowCloseCallback(window, close_callback)
}

close :: proc() {
	glfw.DestroyWindow(window)
	glfw.Terminate()
	window = nil
}

size :: proc() -> geo.V2 {
	w, h := glfw.GetWindowSize(window)
	s := screen_per_point()
	return {f32(w) / s, f32(h) / s}
}

size_px :: proc() -> geo.V2 {
	w, h := glfw.GetFramebufferSize(window)
	return {f32(w), f32(h)}
}

scale :: proc() -> f32 {
	if forced_scale > 0 {return forced_scale}
	sx, _ := glfw.GetWindowContentScale(window)
	return sx
}

////////////////////////////////
//~ fp: OpenGL
//
// The window owns the GL context. Call equip_gl one time after open, and call
// swap one time in each frame. The swap waits for the display, so the vertical
// sync sets the rate of the main loop.

equip_gl :: proc() {
	glfw.MakeContextCurrent(window)
	gl.load_up_to(4, 1, glfw.gl_set_proc_address)
	glfw.SwapInterval(1)
}

swap :: proc() {
	glfw.SwapBuffers(window)
	frame_mark()
}

// The vertical blanks for each swap. A value of 1, which the equip sets,
// presents at each refresh. A value of 2 presents at each second refresh,
// which runs a display of 120Hz at 60. The hardware makes that rate. A limit
// from a timer does not, because the timer and the vertical blank run at rates
// that are near but not equal, and the small difference between the two shows
// as an uneven motion.
set_swap_interval :: proc(interval: int) {
	glfw.SwapInterval(i32(interval))
}

// The refresh rate in Hz of the display. GLFW gives no direct query for the
// monitor that holds a window, so this reads the primary monitor. It is 0
// where no rate exists, and a value of 0 stops the step in frame_mark.
refresh_rate :: proc() -> f32 {
	monitor := glfw.GetPrimaryMonitor()
	if monitor == nil {return 0}
	mode := glfw.GetVideoMode(monitor)
	if mode == nil {return 0}
	return f32(mode.refresh_rate)
}

////////////////////////////////
//~ fp: Frame Timing
//
// The seconds between the last two calls to swap. The swap is the border
// between two frames, so with a vertical sync this value is the period of the
// display. It is 0 until two swaps occur.
//
// Where the rate of the display is known, frame_time gives whole periods of
// the display, and holds the difference from the measured time as a debt. The
// display presents at a vertical blank, so a whole period is the only correct
// unit. A raw measurement moves with the queue of the driver and with the
// scheduler, and that movement shows as an uneven motion on a display whose
// rate does not change.
//
// The debt stays inside approximately one period, so the reported time follows
// the time of the clock. A stall, such as a pause in a debugger or a drag of
// the window, pays out at once as a number of whole periods. The caller
// chooses what to do with such a frame.

@(private)
frame_last: time.Tick
@(private)
frame_dt: f32
@(private)
frame_dt_raw: f32
@(private)
frame_debt: f32 // the seconds of measured time that no frame reported yet

@(private)
frame_mark :: proc() {
	now := time.tick_now()
	if frame_last._nsec != 0 {
		dt := f32(time.duration_seconds(time.tick_diff(frame_last, now)))
		frame_dt_raw = dt
		// Give the time as whole periods of the display, and hold the difference
		// from the measurement as a debt. See the header. In the usual state this
		// code gives exactly one period for each frame, and the movement of the
		// raw measurement changes nothing: a block of 17ms and then a return after
		// 0.5ms, which the queue absorbed, read as two equal periods, which is
		// what the display showed. A true slowdown moves the debt past the limit,
		// and the code then gives more whole periods at once. A stall therefore
		// stays a stall.
		hz := refresh_rate()
		if hz > 0 {
			period := 1.0 / hz
			frame_debt += dt - period
			dt = period
			for frame_debt > 1.5 * period {dt += period; frame_debt -= period}
			for frame_debt < -0.5 * period && dt > 0 {dt -= period; frame_debt += period}
		}
		frame_dt = dt
	}
	frame_last = now
}

frame_time :: proc() -> f32 {
	return frame_dt
}

// the measurement before the step above, for a diagnostic
frame_time_raw :: proc() -> f32 {
	return frame_dt_raw
}

////////////////////////////////
//~ fp: Keys & Events
//
// An event is a change of state. A Key_Down says that the key went down, and
// the key callback removes the automatic repeat of the operating system. A
// Mouse_Down says that a button went down.
//
// The GLFW callbacks put each event on the list, and poll reads the list into
// the digest below. A caller reads the digest and not the list. Put here each
// thing that needs the order of the events, and that the digest cannot hold:
// the text that a person types, and a record of a session to replay.

Key :: enum {
	None,

	//- the letters. They are contiguous, so Key.A + (c - 'a') gives a key.
	A,
	B,
	C,
	D,
	E,
	F,
	G,
	H,
	I,
	J,
	K,
	L,
	M,
	N,
	O,
	P,
	Q,
	R,
	S,
	T,
	U,
	V,
	W,
	X,
	Y,
	Z,

	//- the digits, which are contiguous
	Num0,
	Num1,
	Num2,
	Num3,
	Num4,
	Num5,
	Num6,
	Num7,
	Num8,
	Num9,

	//- the function keys, which are contiguous
	F1,
	F2,
	F3,
	F4,
	F5,
	F6,
	F7,
	F8,
	F9,
	F10,
	F11,
	F12,

	//- the arrows
	Left,
	Right,
	Up,
	Down,

	//- the keys that edit and control
	Escape,
	Space,
	Enter,
	Tab,
	Backspace,
	Delete,
	Insert,
	Home,
	End,
	Page_Up,
	Page_Down,

	//- The modifiers, as keys. The left key and the right key give one value.
	//  Each one makes its own down event and up event. Each other event also
	//  carries the modifiers that are down, in Event.modifiers.
	Shift,
	Ctrl,
	Alt,

	//- the punctuation that a game binds most
	Minus,
	Equals,
	Comma,
	Period,
	Slash,
	Backtick,
}

Mouse_Button :: enum {
	None,
	Left,
	Right,
	Middle,
}

Modifier :: enum {
	Shift,
	Ctrl,
	Alt,
}
Modifiers :: bit_set[Modifier]

Event_Type :: enum {
	None,
	Key_Down,
	Key_Up,
	Mouse_Down,
	Mouse_Up,
	Mouse_Moved,
	Scroll,
	Resize,
	Close_Requested, // the person pressed the close button of the window
}

// Each event carries each member of this struct. `type` says which of them
// hold a value.
Event :: struct {
	type:      Event_Type,
	pos:       geo.V2, // the position of the mouse, in the points of the client area, for a mouse event
	scroll:    geo.V2, // the change of the wheel, for a Scroll. A y above 0 goes away from the person.
	size:      geo.V2, // the new size of the client area, for a Resize
	modifiers: Modifiers, // the modifiers that were down at the event, for a key event and a mouse event
	key:       Key,
	button:    Mouse_Button,
}

@(private)
events: [dynamic]Event

////////////////////////////////
//~ fp: Event Translation

@(private)
key_from_glfw :: proc(key: i32) -> (Key, bool) {
	switch key {
	case glfw.KEY_A ..= glfw.KEY_Z:
		return Key(int(Key.A) + int(key - glfw.KEY_A)), true
	case glfw.KEY_0 ..= glfw.KEY_9:
		return Key(int(Key.Num0) + int(key - glfw.KEY_0)), true
	case glfw.KEY_F1 ..= glfw.KEY_F12:
		return Key(int(Key.F1) + int(key - glfw.KEY_F1)), true

	case glfw.KEY_LEFT:
		return .Left, true
	case glfw.KEY_RIGHT:
		return .Right, true
	case glfw.KEY_UP:
		return .Up, true
	case glfw.KEY_DOWN:
		return .Down, true

	case glfw.KEY_ESCAPE:
		return .Escape, true
	case glfw.KEY_SPACE:
		return .Space, true
	case glfw.KEY_ENTER:
		return .Enter, true
	case glfw.KEY_TAB:
		return .Tab, true
	case glfw.KEY_BACKSPACE:
		return .Backspace, true
	case glfw.KEY_DELETE:
		return .Delete, true
	case glfw.KEY_INSERT:
		return .Insert, true
	case glfw.KEY_HOME:
		return .Home, true
	case glfw.KEY_END:
		return .End, true
	case glfw.KEY_PAGE_UP:
		return .Page_Up, true
	case glfw.KEY_PAGE_DOWN:
		return .Page_Down, true

	case glfw.KEY_LEFT_SHIFT, glfw.KEY_RIGHT_SHIFT:
		return .Shift, true
	case glfw.KEY_LEFT_CONTROL, glfw.KEY_RIGHT_CONTROL:
		return .Ctrl, true
	case glfw.KEY_LEFT_ALT, glfw.KEY_RIGHT_ALT:
		return .Alt, true

	case glfw.KEY_MINUS:
		return .Minus, true
	case glfw.KEY_EQUAL:
		return .Equals, true
	case glfw.KEY_COMMA:
		return .Comma, true
	case glfw.KEY_PERIOD:
		return .Period, true
	case glfw.KEY_SLASH:
		return .Slash, true
	case glfw.KEY_GRAVE_ACCENT:
		return .Backtick, true
	}
	return .None, false
}

@(private)
modifiers_from_glfw :: proc(mods: i32) -> Modifiers {
	result: Modifiers
	if mods & glfw.MOD_SHIFT != 0 {result += {.Shift}}
	if mods & glfw.MOD_CONTROL != 0 {result += {.Ctrl}}
	if mods & glfw.MOD_ALT != 0 {result += {.Alt}}
	return result
}

@(private)
key_callback :: proc "c" (w: glfw.WindowHandle, key, scancode, action, mods: i32) {
	context = runtime.default_context()
	if action == glfw.REPEAT {return} 	// the digest wants the edges, without the automatic repeat
	k, ok := key_from_glfw(key)
	if !ok {return}
	type := Event_Type.Key_Down if action == glfw.PRESS else Event_Type.Key_Up
	append(&events, Event{type = type, key = k, modifiers = modifiers_from_glfw(mods)})
}

@(private)
mouse_button_callback :: proc "c" (w: glfw.WindowHandle, button, action, mods: i32) {
	context = runtime.default_context()
	b: Mouse_Button
	switch button {
	case glfw.MOUSE_BUTTON_LEFT:
		b = .Left
	case glfw.MOUSE_BUTTON_RIGHT:
		b = .Right
	case glfw.MOUSE_BUTTON_MIDDLE:
		b = .Middle
	case:
		return
	}
	x, y := glfw.GetCursorPos(w)
	s := screen_per_point()
	type := Event_Type.Mouse_Down if action == glfw.PRESS else Event_Type.Mouse_Up
	append(
		&events,
		Event {
			type = type,
			button = b,
			pos = {f32(x) / s, f32(y) / s},
			modifiers = modifiers_from_glfw(mods),
		},
	)
}

@(private)
cursor_pos_callback :: proc "c" (w: glfw.WindowHandle, x, y: f64) {
	context = runtime.default_context()
	s := screen_per_point()
	append(&events, Event{type = .Mouse_Moved, pos = {f32(x) / s, f32(y) / s}})
}

@(private)
scroll_callback :: proc "c" (w: glfw.WindowHandle, dx, dy: f64) {
	context = runtime.default_context()
	x, y := glfw.GetCursorPos(w)
	s := screen_per_point()
	// The scroll is in steps of the wheel and not in screen units, so it does
	// not convert.
	append(&events, Event{type = .Scroll, scroll = {f32(dx), f32(dy)}, pos = {f32(x) / s, f32(y) / s}})
}

@(private)
size_callback :: proc "c" (w: glfw.WindowHandle, width, height: i32) {
	context = runtime.default_context()
	s := screen_per_point()
	append(&events, Event{type = .Resize, size = {f32(width) / s, f32(height) / s}})
}

@(private)
close_callback :: proc "c" (w: glfw.WindowHandle) {
	context = runtime.default_context()
	append(&events, Event{type = .Close_Requested})
}

////////////////////////////////
//~ fp: Input
//
// The digest of the stream of events, which stays equal across a frame. poll
// reads the list one time. The state of each key and each edge then reads the
// same in each place, for the whole frame.
//
// `pressed` and `released` hold their value for the frame. A button that went
// down at any point of the frame therefore reads as pressed, and it does so
// after it comes up again inside that same frame.
//
// Each query gives the state as of the last poll. No other function reads the
// queue of the operating system.

@(private)
Input_State :: struct {
	key_down:        [Key]bool,
	key_pressed:     [Key]bool, // It holds for the frame: the key went down at some point of this frame.
	mouse_down:      [Mouse_Button]bool,
	mouse_pressed:   [Mouse_Button]bool, // It holds for the frame, as key_pressed does.
	mouse_released:  [Mouse_Button]bool, // It holds for the frame: the button went up at some point of this frame.
	close_requested: bool,
	mouse_pos:       geo.V2,
	scroll:          geo.V2, // the sum across the frame, because the wheel can make more than one event
}

@(private)
input: Input_State

// one time in each frame, before each query below
poll :: proc() {
	// Clear the members that hold for one frame BEFORE the events of this frame
	// arrive. Without that order a caller could never read an edge.
	input.key_pressed = {}
	input.mouse_pressed = {}
	input.mouse_released = {}
	input.scroll = {}
	input.close_requested = false

	glfw.PollEvents() // it runs the callbacks above, which fill the list

	for evt in events {
		switch evt.type {
		case .Key_Down:
			input.key_down[evt.key] = true
			input.key_pressed[evt.key] = true // a Key_Up in this same frame does not clear it
		case .Key_Up:
			input.key_down[evt.key] = false
		case .Mouse_Down:
			input.mouse_down[evt.button] = true
			input.mouse_pressed[evt.button] = true // a Mouse_Up in this same frame does not clear it
		case .Mouse_Up:
			input.mouse_down[evt.button] = false
			input.mouse_released[evt.button] = true // a Mouse_Down in this same frame does not clear it
		case .Mouse_Moved:
			input.mouse_pos = evt.pos
		case .Scroll:
			input.scroll += evt.scroll
		case .Close_Requested:
			input.close_requested = true
		case .Resize, .None:
		}
	}
	clear(&events)
}

// the close button went down in this frame
close_requested :: proc() -> bool {
	return input.close_requested
}

key_down :: proc(key: Key) -> bool {
	return input.key_down[key]
}

key_pressed :: proc(key: Key) -> bool {
	return input.key_pressed[key]
}

mouse_down :: proc(button: Mouse_Button) -> bool {
	return input.mouse_down[button]
}

mouse_pressed :: proc(button: Mouse_Button) -> bool {
	return input.mouse_pressed[button]
}

mouse_released :: proc(button: Mouse_Button) -> bool {
	return input.mouse_released[button]
}

mouse_pos :: proc() -> geo.V2 {
	return input.mouse_pos
}

// the change of the wheel across this frame. A y above 0 goes away from the person.
scroll :: proc() -> geo.V2 {
	return input.scroll
}


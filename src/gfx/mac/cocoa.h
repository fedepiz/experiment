#pragma once

////////////////////////////////
//~ fp: Cocoa Compatibility Layer -- C boundary
//
// The one piece of the project outside the unity build: Cocoa wants
// Objective-C, so cocoa.m implements this API as its own TU, compiled and
// linked in by x.sh on Darwin only. The boundary is deliberately dumb --
// plain C types, raw hardware keycodes, no base-layer types -- so the ObjC
// side needs nothing from the rest of src/, and every translation into
// codebase idiom (WND_Key tables, arena-pushed events) happens on the C side,
// in mac/window.c.
//
// Coordinates cross the boundary as client-area points with a top-left
// origin; the flip from Cocoa's bottom-left convention happens in cocoa.m.

typedef int Cocoa_EventKind;
enum {
  Cocoa_EventKind_Nil = 0,
  Cocoa_EventKind_KeyDown,
  Cocoa_EventKind_KeyUp,
  Cocoa_EventKind_MouseDown,
  Cocoa_EventKind_MouseUp,
  Cocoa_EventKind_MouseMoved,
  Cocoa_EventKind_Scroll,
  Cocoa_EventKind_Resize,
  Cocoa_EventKind_CloseRequested,
};

// OR'd together
enum {
  Cocoa_Modifier_Shift = (1 << 0),
  Cocoa_Modifier_Ctrl  = (1 << 1),
  Cocoa_Modifier_Alt   = (1 << 2),
};

// Fat struct, like WND_Event: `kind` says which fields mean anything.
typedef struct {
  Cocoa_EventKind kind;
  unsigned short keycode;   // raw kVK hardware keycode (key events)
  int button;               // 0 left / 1 right / 2 middle (mouse events)
  unsigned int modifiers;   // Cocoa_Modifier_* held at event time
  float x, y;               // mouse position, client points, top-left origin
  float scroll_x, scroll_y; // wheel delta; +y is away from the user
  float width, height;      // new client size (Resize)
} Cocoa_Event;

// title need not be null-terminated; title_len says how many bytes matter
void cocoa_window_open(const char* title, int title_len, int width, int height);
void cocoa_window_close(void);
void cocoa_window_size(float* out_width, float* out_height);

// Drains the AppKit queue into an internal buffer (bounded; resize floods
// coalesce, anything past the cap drops). Call once per frame, then pop with
// cocoa_next_event until it returns 0.
void cocoa_pump_events(void);
int  cocoa_next_event(Cocoa_Event* out);

// OpenGL: equip attaches a double-buffered, vsynced GL context (4.1 core, the
// most macOS gives) to the open window and makes it current on the calling
// thread; swap presents and blocks to the display rate. The framebuffer is
// in pixels, the window in points; backing_scale is pixels per point (2 on
// retina).
void  cocoa_gl_equip(void);
void  cocoa_gl_swap(void);
void  cocoa_gl_set_swap_interval(int interval);
void  cocoa_framebuffer_size(float* out_width, float* out_height);
float cocoa_backing_scale(void);

// nominal refresh rate of the screen the window sits on, in Hz; 0 when there
// is no window or no screen (re-resolves on every call, so a drag between
// mixed-rate monitors updates the answer)
float cocoa_refresh_rate(void);

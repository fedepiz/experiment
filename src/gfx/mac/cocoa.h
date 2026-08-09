#pragma once

////////////////////////////////
//~ fp: Cocoa Compatibility Layer -- C boundary
//
// This is the one part of the project outside the unity build. Cocoa needs
// Objective-C, so cocoa.m implements this interface as its own translation
// unit. x.sh compiles that file and links it on Darwin only.
//
// This interface holds plain C types, a raw hardware key code, and no type of
// the base layer. The Objective-C side therefore needs nothing else from src/.
// Each step into the style of this codebase, such as the tables of WND_Key and
// the events on an arena, is on the C side, in mac/window.c.
//
// A coordinate crosses this boundary as a point of the client area, from the
// top left corner. cocoa.m makes the change from the bottom left corner, which
// Cocoa uses.

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

// The code puts these values together with an OR.
enum {
  Cocoa_Modifier_Shift = (1 << 0),
  Cocoa_Modifier_Ctrl  = (1 << 1),
  Cocoa_Modifier_Alt   = (1 << 2),
};

// This struct carries each member, as WND_Event does. `kind` says which of
// them hold a value.
typedef struct {
  Cocoa_EventKind kind;
  unsigned short keycode;   // the raw kVK hardware key code, for a key event
  int button;               // 0 is left, 1 is right and 2 is middle, for a mouse event
  unsigned int modifiers;   // the Cocoa_Modifier_ values that were down at the event
  float x, y;               // the position of the mouse, in the points of the client area, from the top left corner
  float scroll_x, scroll_y; // the change of the wheel. A y above 0 goes away from the person.
  float width, height;      // the new size of the client area, for a Resize
} Cocoa_Event;

// The title needs no null byte at its end. title_len gives its length in
// bytes.
void cocoa_window_open(const char* title, int title_len, int width, int height);
void cocoa_window_close(void);
void cocoa_window_size(float* out_width, float* out_height);

// Move each event of the queue of AppKit into an internal buffer. That buffer
// has a limit. A long run of resize events becomes one event, and an event
// past the limit drops. Call this function one time in each frame, then call
// cocoa_next_event until it gives 0.
void cocoa_pump_events(void);
int  cocoa_next_event(Cocoa_Event* out);

// The equip attaches a GL context to the open window, and makes that context
// current on the thread of the caller. The context has two buffers and a
// vertical sync, and its version is 4.1 core, which is the highest version
// that macOS gives. The swap presents the image, and waits for the display.
//
// The framebuffer is in pixels, and the window is in points. backing_scale is
// the pixels for each point, which is 2 on a screen of a high density.
void  cocoa_gl_equip(void);
void  cocoa_gl_swap(void);
void  cocoa_gl_set_swap_interval(int interval);
void  cocoa_framebuffer_size(float* out_width, float* out_height);
float cocoa_backing_scale(void);

// The nominal refresh rate in Hz of the screen that holds the window. It is 0
// where there is no window, and where there is no screen. This function asks
// the system at each call, so a drag between two monitors of different rates
// changes the answer.
float cocoa_refresh_rate(void);

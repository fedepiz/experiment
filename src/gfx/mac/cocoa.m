#include "cocoa.h"

////////////////////////////////
//~ fp: Cocoa Compatibility Layer -- Objective-C side
//
// Compiled as its own TU (see x.sh), so none of this -- the Cocoa import, the
// ObjC dialect, AppKit's typedef soup -- ever touches the unity build. Plain
// `static` instead of `internal` here: base/core.h is deliberately not
// included, this file depends only on cocoa.h and the system.

#import <Cocoa/Cocoa.h>
#include <string.h>

// NSOpenGL* is deprecated (Apple would rather we used Metal); the GL path is
// a deliberate choice, so silence the nagging. Revisit if a Metal backend
// ever lands.
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

#define COCOA_EVENT_CAP 1024

static struct {
  NSWindow* window;
  id delegate;
  NSOpenGLContext* gl_context;
  int is_open;
  Cocoa_Event events[COCOA_EVENT_CAP];
  int events_count; // write cursor
  int events_read;  // read cursor; pump compacts the unread tail to the front
} cocoa_state;

// All pushes happen inside cocoa_pump_events (delegate callbacks included:
// they fire within sendEvent), so pump-then-drain never races the buffer.
static void cocoa__push_event(Cocoa_Event e) {
  // live-resize delivers one Resize per frame for seconds on end -- only the
  // latest matters, so consecutive Resizes coalesce
  if(e.kind == Cocoa_EventKind_Resize && cocoa_state.events_count > cocoa_state.events_read &&
     cocoa_state.events[cocoa_state.events_count - 1].kind == Cocoa_EventKind_Resize) {
    cocoa_state.events[cocoa_state.events_count - 1] = e;
  } else if(cocoa_state.events_count < COCOA_EVENT_CAP) {
    cocoa_state.events[cocoa_state.events_count] = e;
    cocoa_state.events_count += 1;
  }
  // full: drop; a thousand events in one frame means the frame is the problem
}

static unsigned int cocoa__modifiers_from_flags(NSEventModifierFlags flags) {
  unsigned int result = 0;
  if(flags & NSEventModifierFlagShift)   { result |= Cocoa_Modifier_Shift; }
  if(flags & NSEventModifierFlagControl) { result |= Cocoa_Modifier_Ctrl; }
  if(flags & NSEventModifierFlagOption)  { result |= Cocoa_Modifier_Alt; }
  return result;
}

// window coords -> client coords: into the content view, then flip to
// top-left origin
static void cocoa__client_pos_from_event(NSEvent* ev, float* out_x, float* out_y) {
  NSView* view = [cocoa_state.window contentView];
  NSPoint p = [view convertPoint:[ev locationInWindow] fromView:nil];
  *out_x = (float)p.x;
  *out_y = (float)([view bounds].size.height - p.y);
}

//- fp: window delegate: the close button asks instead of killing the window
//  (mirroring X11's WM_DELETE_WINDOW protocol), and resizes arrive here
//  rather than through the event queue
@interface Cocoa_WindowDelegate : NSObject <NSWindowDelegate>
@end

@implementation Cocoa_WindowDelegate
- (BOOL)windowShouldClose:(NSWindow*)sender {
  (void)sender;
  Cocoa_Event e = {0};
  e.kind = Cocoa_EventKind_CloseRequested;
  cocoa__push_event(e);
  return NO;
}
- (void)windowDidResize:(NSNotification*)notification {
  (void)notification;
  // the GL context tracks its view's surface lazily; tell it the geometry moved
  [cocoa_state.gl_context update];
  NSSize size = [[cocoa_state.window contentView] frame].size;
  Cocoa_Event e = {0};
  e.kind = Cocoa_EventKind_Resize;
  e.width = (float)size.width;
  e.height = (float)size.height;
  cocoa__push_event(e);
}
- (void)windowDidMove:(NSNotification*)notification {
  (void)notification;
  [cocoa_state.gl_context update];
}
@end

void cocoa_window_open(const char* title, int title_len, int width, int height) {
  @autoreleasepool {
    [NSApplication sharedApplication];
    // a bare executable (no .app bundle) is a background process by default;
    // Regular gives it a Dock icon and lets its window become key
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

    NSWindowStyleMask style_mask = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                   NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
    NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, (CGFloat)width, (CGFloat)height)
                                                   styleMask:style_mask
                                                     backing:NSBackingStoreBuffered
                                                       defer:NO];
    [window setReleasedWhenClosed:NO]; // we own the lifetime; cocoa_window_close releases

    NSString* ns_title = [[[NSString alloc] initWithBytes:title
                                                   length:(NSUInteger)title_len
                                                 encoding:NSUTF8StringEncoding] autorelease];
    [window setTitle:ns_title];

    Cocoa_WindowDelegate* delegate = [[Cocoa_WindowDelegate alloc] init];
    [window setDelegate:delegate];
    [window setAcceptsMouseMovedEvents:YES]; // MouseMoved is opt-in on Cocoa

    [window center];
    [window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    [NSApp finishLaunching];

    cocoa_state.window = window;
    cocoa_state.delegate = delegate;
    cocoa_state.is_open = 1;
  }
}

void cocoa_window_close(void) {
  if(cocoa_state.is_open) {
    @autoreleasepool {
      if(cocoa_state.gl_context != nil) {
        [NSOpenGLContext clearCurrentContext];
        [cocoa_state.gl_context release];
      }
      [cocoa_state.window setDelegate:nil];
      [cocoa_state.window close];
      [cocoa_state.window release];
      [cocoa_state.delegate release];
    }
    memset(&cocoa_state, 0, sizeof(cocoa_state));
  }
}

void cocoa_window_size(float* out_width, float* out_height) {
  float w = 0, h = 0;
  if(cocoa_state.is_open) {
    NSSize size = [[cocoa_state.window contentView] frame].size;
    w = (float)size.width;
    h = (float)size.height;
  }
  if(out_width)  { *out_width = w; }
  if(out_height) { *out_height = h; }
}

void cocoa_pump_events(void) {
  if(!cocoa_state.is_open) { return; }

  // compact any unread tail to the front (normally there is none)
  if(cocoa_state.events_read > 0) {
    int remaining = cocoa_state.events_count - cocoa_state.events_read;
    memmove(cocoa_state.events, cocoa_state.events + cocoa_state.events_read,
            (size_t)remaining * sizeof(Cocoa_Event));
    cocoa_state.events_count = remaining;
    cocoa_state.events_read = 0;
  }

  @autoreleasepool {
    for(;;) {
      NSEvent* ev = [NSApp nextEventMatchingMask:NSEventMaskAny
                                       untilDate:[NSDate distantPast]
                                          inMode:NSDefaultRunLoopMode
                                         dequeue:YES];
      if(ev == nil) { break; }

      int forward = 1;
      switch([ev type]) {
        case NSEventTypeKeyDown:
        case NSEventTypeKeyUp: {
          // swallowed: forwarding a keyDown nothing responds to makes Cocoa beep
          forward = 0;
          Cocoa_Event e = {0};
          e.kind = ([ev type] == NSEventTypeKeyDown) ? Cocoa_EventKind_KeyDown : Cocoa_EventKind_KeyUp;
          e.keycode = [ev keyCode];
          e.modifiers = cocoa__modifiers_from_flags([ev modifierFlags]);
          cocoa__push_event(e);
        } break;

        case NSEventTypeFlagsChanged: {
          // modifier presses arrive here, not as keyDown/keyUp; whether the
          // flag is set after the change tells press from release (releasing
          // one of two held shifts mis-reports as a press -- X11's collapsed
          // left/right pairs share the blind spot)
          unsigned short code = [ev keyCode];
          NSEventModifierFlags flag =
            (code == 0x38 || code == 0x3C) ? NSEventModifierFlagShift :   // kVK_(Right)Shift
            (code == 0x3B || code == 0x3E) ? NSEventModifierFlagControl : // kVK_(Right)Control
            (code == 0x3A || code == 0x3D) ? NSEventModifierFlagOption :  // kVK_(Right)Option
                                             0;
          if(flag != 0) {
            NSEventModifierFlags flags = [ev modifierFlags];
            Cocoa_Event e = {0};
            e.kind = (flags & flag) ? Cocoa_EventKind_KeyDown : Cocoa_EventKind_KeyUp;
            e.keycode = code;
            e.modifiers = cocoa__modifiers_from_flags(flags);
            cocoa__push_event(e);
          }
        } break;

        case NSEventTypeLeftMouseDown:
        case NSEventTypeLeftMouseUp:
        case NSEventTypeRightMouseDown:
        case NSEventTypeRightMouseUp:
        case NSEventTypeOtherMouseDown:
        case NSEventTypeOtherMouseUp: {
          if([ev window] == cocoa_state.window) {
            NSEventType t = [ev type];
            // buttonNumber already speaks our convention: 0 left, 1 right, 2 middle
            int button = (int)[ev buttonNumber];
            if(button <= 2) {
              int is_down = (t == NSEventTypeLeftMouseDown || t == NSEventTypeRightMouseDown ||
                             t == NSEventTypeOtherMouseDown);
              Cocoa_Event e = {0};
              e.kind = is_down ? Cocoa_EventKind_MouseDown : Cocoa_EventKind_MouseUp;
              e.button = button;
              e.modifiers = cocoa__modifiers_from_flags([ev modifierFlags]);
              cocoa__client_pos_from_event(ev, &e.x, &e.y);
              // titlebar clicks belong to the system, not the client (X11
              // parity: decorations never reach the app). Only downs filter
              // -- an up after dragging out of the window must still arrive.
              NSSize bounds = [[cocoa_state.window contentView] bounds].size;
              int inside = (e.x >= 0 && e.y >= 0 &&
                            e.x <= (float)bounds.width && e.y <= (float)bounds.height);
              if(!is_down || inside) { cocoa__push_event(e); }
            }
          }
        } break;

        case NSEventTypeMouseMoved:
        case NSEventTypeLeftMouseDragged:
        case NSEventTypeRightMouseDragged:
        case NSEventTypeOtherMouseDragged: {
          // dragged variants fold into MouseMoved, matching X11's MotionNotify
          if([ev window] == cocoa_state.window) {
            Cocoa_Event e = {0};
            e.kind = Cocoa_EventKind_MouseMoved;
            e.modifiers = cocoa__modifiers_from_flags([ev modifierFlags]);
            cocoa__client_pos_from_event(ev, &e.x, &e.y);
            cocoa__push_event(e);
          }
        } break;

        case NSEventTypeScrollWheel: {
          if([ev window] == cocoa_state.window) {
            float dx = (float)[ev scrollingDeltaX];
            float dy = (float)[ev scrollingDeltaY]; // +y away from user already
            // wheels report lines, trackpads report points; squash points to
            // roughly line-sized steps so both feel the same past the boundary
            if([ev hasPreciseScrollingDeltas]) { dx *= 0.1f; dy *= 0.1f; }
            Cocoa_Event e = {0};
            e.kind = Cocoa_EventKind_Scroll;
            e.scroll_x = dx;
            e.scroll_y = dy;
            e.modifiers = cocoa__modifiers_from_flags([ev modifierFlags]);
            cocoa__client_pos_from_event(ev, &e.x, &e.y);
            cocoa__push_event(e);
          }
        } break;

        default: break;
      }
      if(forward) { [NSApp sendEvent:ev]; }
    }
  }
}

int cocoa_next_event(Cocoa_Event* out) {
  int result = 0;
  if(cocoa_state.events_read < cocoa_state.events_count) {
    *out = cocoa_state.events[cocoa_state.events_read];
    cocoa_state.events_read += 1;
    result = 1;
  }
  return result;
}

////////////////////////////////
//~ fp: OpenGL

void cocoa_gl_equip(void) {
  if(!cocoa_state.is_open || cocoa_state.gl_context != nil) { return; }
  @autoreleasepool {
    NSOpenGLPixelFormatAttribute attrs[] = {
      NSOpenGLPFAOpenGLProfile, NSOpenGLProfileVersion4_1Core,
      NSOpenGLPFADoubleBuffer,
      NSOpenGLPFAColorSize, 24,
      NSOpenGLPFAAlphaSize, 8,
      NSOpenGLPFAAccelerated,
      0,
    };
    NSOpenGLPixelFormat* format = [[[NSOpenGLPixelFormat alloc] initWithAttributes:attrs] autorelease];
    NSOpenGLContext* context = [[NSOpenGLContext alloc] initWithFormat:format shareContext:nil];

    NSView* view = [cocoa_state.window contentView];
    [view setWantsBestResolutionOpenGLSurface:YES]; // retina-sized backbuffer, not 1x
    [context setView:view];
    [context makeCurrentContext];

    GLint one = 1; // vsync: flushBuffer blocks to the display rate
    [context setValues:&one forParameter:NSOpenGLContextParameterSwapInterval];

    cocoa_state.gl_context = context;
  }
}

void cocoa_gl_swap(void) {
  [cocoa_state.gl_context flushBuffer];
}

void cocoa_gl_set_swap_interval(int interval) {
  if(cocoa_state.gl_context != nil) {
    GLint value = interval;
    [cocoa_state.gl_context setValues:&value forParameter:NSOpenGLContextParameterSwapInterval];
  }
}

void cocoa_framebuffer_size(float* out_width, float* out_height) {
  float w = 0, h = 0;
  if(cocoa_state.is_open) {
    NSView* view = [cocoa_state.window contentView];
    NSSize size = [view convertRectToBacking:[view bounds]].size;
    w = (float)size.width;
    h = (float)size.height;
  }
  if(out_width)  { *out_width = w; }
  if(out_height) { *out_height = h; }
}

float cocoa_backing_scale(void) {
  float result = 1.0f;
  if(cocoa_state.is_open) {
    result = (float)[cocoa_state.window backingScaleFactor];
  }
  return result;
}

float cocoa_refresh_rate(void) {
  float result = 0;
  if(cocoa_state.is_open) {
    @autoreleasepool {
      // maximumFramesPerSecond rather than CGDisplayModeGetRefreshRate: the
      // CG call reports 0 on built-in panels, and this one already speaks the
      // nominal rate ProMotion paces to (120). [window screen] tracks the
      // screen the window mostly sits on, so no per-monitor cache is needed
      NSScreen* screen = [cocoa_state.window screen];
      if(screen != nil) {
        result = (float)[screen maximumFramesPerSecond];
      }
    }
  }
  return result;
}

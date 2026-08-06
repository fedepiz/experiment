#pragma once

#include "base/core.h"
#include "base/math.h"
#include "base/arena.h"
#include "base/strings.h"

////////////////////////////////
//~ fp: UI
//
// Immediate-mode UI core, depending on base only. Input arrives as a UI_Input
// value, drawing leaves as a flat UI_DrawCommand array, and text measurement
// goes through callbacks installed at ui_init. Frame protocol:
//
//   ui_frame_begin(frame_arena, input);
//   ... build boxes / widgets, query ui_signal in any order ...
//   UI_DrawList list = ui_frame_end(); // layout solve + command emission
//
// Boxes form a tree rebuilt from scratch each frame on the frame arena.
// Cross-frame state -- hover/press animation, the previous frame's rects --
// persists in an internal table addressed by box key, so interaction queries
// during the build test against the previous frame's layout (one frame
// stale). All coordinates are points, top-left origin; colors are V4 RGBA in
// 0..1, matching the draw layer's conventions without depending on it.

////////////////////////////////
//~ fp: Keys
//
// A box's key names it across frames. Keys hash from the box's string, mixed
// with the parent box's key and the top of the seed stack -- push a seed
// (loop index, entity id) to disambiguate identical strings built in a loop.
//
// String conventions, borrowed from Dear ImGui / RF:
//   "Label"          hash of the whole string; all of it displays
//   "Label##suffix"  hash of the whole string; only "Label" displays
//   "Label###suffix" hash of "suffix" alone; only "Label" displays
//                    (display text can change without changing identity)
// A string with an empty hash part yields key 0: a transient box with no
// persistent state and no interaction.

typedef U64 UI_Key;

////////////////////////////////
//~ fp: Semantic Sizes
//
// Each box states a preferred size per axis; ui_frame_end solves the tree.
// strictness in 0..1 is the fraction of the preferred size the box refuses
// to give up when its parent overflows: 1 never shrinks, 0 shrinks freely.

typedef U32 UI_Axis;
enum {
  UI_Axis_X,
  UI_Axis_Y,
  UI_Axis_COUNT,
};

typedef U32 UI_SizeKind;
enum {
  UI_SizeKind_ChildrenSum = 0, // sum of children along the child axis, max
                               // across it; the ZII default
  UI_SizeKind_Points,          // value points, as-is
  UI_SizeKind_Text,            // the box's text: natural width / line-count
                               // height (after wrapping, if the box wraps)
  UI_SizeKind_PctOfParent,     // value (0..1) times the parent's content size
                               // (undefined when the parent sizes by children)
  UI_SizeKind_Grow,            // starts at zero, then takes value/(sum of
                               // sibling grow values) of the parent's leftover
                               // space along the parent's child axis; across
                               // that axis it fills the parent's content
};

typedef struct {
  UI_SizeKind kind;
  F32 value;
  F32 strictness;
} UI_Size;

internal UI_Size ui_size_px(F32 points, F32 strictness);
internal UI_Size ui_size_pct(F32 pct, F32 strictness);
internal UI_Size ui_size_text(F32 strictness);
internal UI_Size ui_size_children(F32 strictness);
internal UI_Size ui_size_grow(F32 weight);

////////////////////////////////
//~ fp: Boxes
//
// The one node type: widgets are flag combinations plus style, composed by
// the widget functions below or by callers directly. A box's style fields
// are captured from the stacks at build time and stay writable until
// ui_frame_end reads them.

typedef U32 UI_BoxFlags;
enum {
  UI_BoxFlag_Clickable      = (1 << 0), // hit-testable; hover/press animate
  UI_BoxFlag_DrawBackground = (1 << 1),
  UI_BoxFlag_DrawBorder     = (1 << 2),
  UI_BoxFlag_DrawText       = (1 << 3),
  UI_BoxFlag_TextWrap       = (1 << 4), // wrap text to the solved width;
                                        // without it text breaks on '\n' only
  UI_BoxFlag_Clip           = (1 << 5), // clip children and text to the box
  UI_BoxFlag_Floating       = (1 << 6), // out of layout flow: positioned at
                                        // floating_pos relative to the parent
};

typedef U32 UI_TextAlign;
enum {
  UI_TextAlign_Left = 0,
  UI_TextAlign_Center,
  UI_TextAlign_Right,
};

// placement of a box's children across its child axis (the cross axis):
// a row's children align vertically, a column's horizontally
typedef U32 UI_Align;
enum {
  UI_Align_Start = 0, // at the padding edge (ZII)
  UI_Align_Center,
  UI_Align_End,
};

typedef struct UI_BoxState UI_BoxState; // ui-internal persistent state

typedef struct UI_Box UI_Box;
struct UI_Box {
  //- tree links; frame lifetime
  UI_Box* parent;
  UI_Box* first;
  UI_Box* last;
  UI_Box* next;
  UI_Box* prev;

  //- identity
  UI_Key key; // 0 = transient
  UI_BoxFlags flags;
  String8 string; // display part, copied to the frame arena

  //- style, captured from the stacks at build
  UI_Size pref_size[UI_Axis_COUNT];
  UI_Axis child_axis;   // children lay out along this axis
  UI_Align child_align; // and align like this across it
  U64 font;
  F32 font_size;
  V4 text_color;
  UI_TextAlign text_align;
  V4 background_color;
  V4 border_color;
  F32 border_thickness;
  F32 corner_radius;
  V2 padding;         // per-side inset between the box and its content
  F32 child_gap;      // points between adjacent children
  V2 floating_pos;    // Floating boxes: offset applied after anchoring
  V2 floating_anchor; // Floating boxes: per-axis 0..1 fraction naming the
                      // same point on the parent and on the box; the box is
                      // placed so the two coincide, then floating_pos offsets
                      // it. {0,0} top-left on top-left (the ZII default),
                      // {1,1} bottom-right on bottom-right, {0.5,0.5} centered.

  //- solved by ui_frame_end; garbage before it
  F32 fixed_size[UI_Axis_COUNT];
  Rect rect;
  String8* lines; // final drawn lines (DrawText boxes)
  U64 line_count;

  UI_BoxState* state; // nil for transient boxes
};

////////////////////////////////
//~ fp: Input
//
// Filled by the caller each frame from whatever input system it owns. Mouse
// position is in points. pressed/released are frame edges; down is held
// state. dt is the frame delta in seconds (drives animation); window is the
// root box size in points.

typedef U8 UI_MouseButton;
enum {
  UI_MouseButton_Left = 0,
  UI_MouseButton_Right,
  UI_MouseButton_Middle,
  UI_MouseButton_COUNT,
};

typedef struct {
  V2 mouse;
  B8 down[UI_MouseButton_COUNT];
  B8 pressed[UI_MouseButton_COUNT];
  B8 released[UI_MouseButton_COUNT];
  V2 scroll;
  F32 dt;
  V2 window;
} UI_Input;

////////////////////////////////
//~ fp: Text Callbacks
//
// Fonts are opaque U64 handles the callbacks interpret; the core never
// decodes them. measure returns the bounding-box dimensions of a single-line
// run. Measurements are cached internally by (font, size, string), so the
// callback only runs on cache misses.

typedef struct {
  F32 ascent;       // top of box -> baseline
  F32 descent;      // baseline -> bottom of box
  F32 line_advance; // baseline -> next baseline
} UI_FontMetrics;

typedef V2 UI_MeasureTextFunc(void* user, U64 font, F32 size, String8 text);
typedef UI_FontMetrics UI_FontMetricsFunc(void* user, U64 font, F32 size);

////////////////////////////////
//~ fp: Roles / Theme
//
// Widgets style themselves by role: the theme holds one UI_RoleStyle per
// role, installed together via ui_set_theme (typically loaded from data at
// startup; ui_init installs ui_default_theme). Per property, precedence at
// box build is: a style stack pushed above its default depth trumps the
// role; an untouched stack defers to it; fields written on the returned box
// afterwards trump both. The stacks' own defaults are the Default role's
// values, so plain boxes and role boxes read identically until something is
// pushed.

typedef U32 UI_Role;
enum {
  UI_Role_Default = 0, // plain boxes, labels; also seeds the stack defaults
  UI_Role_Panel,
  UI_Role_Button,
  UI_Role_Tooltip,
  UI_Role_COUNT,
};

typedef struct {
  V4 background_color;
  V4 text_color;
  V4 border_color;
  F32 border_thickness;
  F32 corner_radius;
} UI_RoleStyle;

typedef struct {
  F32 font_size; // ambient, not per-role
  UI_RoleStyle roles[UI_Role_COUNT];
  // named colors no widget reads; for call sites (highlight values,
  // secondary text, ...)
  V4 accent_color;
  V4 muted_color;
} UI_Theme;

internal UI_Theme  ui_default_theme(void);
internal void      ui_set_theme(UI_Theme theme);
internal UI_Theme* ui_theme(void); // the installed theme, for call-site reads

////////////////////////////////
//~ fp: Draw Commands
//
// ui_frame_end's output: a flat array on the frame arena, in paint order.
// Rect mirrors the draw layer's rect params; Text is one line, no newlines,
// pos at the top-left of the line's bounding box. ClipPush rects nest by
// intersection and every push has a matching pop.

typedef U32 UI_DrawCommandKind;
enum {
  UI_DrawCommandKind_Nil = 0,
  UI_DrawCommandKind_Rect,
  UI_DrawCommandKind_Text,
  UI_DrawCommandKind_ClipPush,
  UI_DrawCommandKind_ClipPop,
};

typedef struct {
  UI_DrawCommandKind kind;

  //- Rect / ClipPush
  Rect rect;
  V4 colors[Corner_COUNT];
  F32 corner_radii[Corner_COUNT];
  F32 border_thickness; // 0 = filled
  F32 edge_softness;

  //- Text
  U64 font;
  F32 font_size;
  V2 pos;
  V4 color;
  String8 text;
} UI_DrawCommand;

typedef struct {
  UI_DrawCommand* commands;
  U64 count;
  B32 mouse_over_ui; // mouse is inside a clickable or background-drawing
                     // box, or a press that started on one is still held
} UI_DrawList;

////////////////////////////////
//~ fp: Frame Lifecycle

internal void        ui_init(UI_MeasureTextFunc* measure, UI_FontMetricsFunc* metrics, void* user);
internal void        ui_frame_begin(Arena* frame_arena, UI_Input input);
internal UI_DrawList ui_frame_end(void);

internal UI_Box* ui_root(void);  // the window-sized root box of this frame
internal V2      ui_mouse(void); // this frame's mouse, in points

////////////////////////////////
//~ fp: Style Stacks
//
// Every stack resets at ui_frame_begin to a ZII-friendly default; boxes
// capture the tops at build time. Pushes must balance pops within the frame
// (asserted).

internal void ui_push_parent(UI_Box* box);
internal void ui_pop_parent(void);
internal void ui_push_seed(U64 seed);
internal void ui_pop_seed(void);
internal void ui_push_pref_width(UI_Size size);
internal void ui_pop_pref_width(void);
internal void ui_push_pref_height(UI_Size size);
internal void ui_pop_pref_height(void);
internal void ui_push_font(U64 font);
internal void ui_pop_font(void);
internal void ui_push_font_size(F32 size);
internal void ui_pop_font_size(void);
internal void ui_push_text_color(V4 color);
internal void ui_pop_text_color(void);
internal void ui_push_text_align(UI_TextAlign align);
internal void ui_pop_text_align(void);
internal void ui_push_background_color(V4 color);
internal void ui_pop_background_color(void);
internal void ui_push_border_color(V4 color);
internal void ui_pop_border_color(void);
internal void ui_push_border_thickness(F32 thickness);
internal void ui_pop_border_thickness(void);
internal void ui_push_corner_radius(F32 radius);
internal void ui_pop_corner_radius(void);
internal void ui_push_child_axis(UI_Axis axis);
internal void ui_pop_child_axis(void);
internal void ui_push_child_align(UI_Align align);
internal void ui_pop_child_align(void);
internal void ui_push_padding(V2 padding);
internal void ui_pop_padding(void);
internal void ui_push_child_gap(F32 gap);
internal void ui_pop_child_gap(void);

////////////////////////////////
//~ fp: Building / Signals
//
// ui_box appends a box under the current parent and returns it for field
// tweaks; ui_signal reads the box's interaction against the previous frame's
// layout. hovered follows the topmost clickable box under the mouse;
// clicked = left button released over the box it was pressed on.

typedef struct {
  UI_Box* box; // the box the signal was read from, for field tweaks
  B8 hovered;
  B8 pressed;  // left went down on it this frame
  B8 down;     // left held since pressing it
  B8 released; // left came up this frame, wherever the mouse is
  B8 clicked;
  B8 right_clicked;
  V2 drag_delta; // mouse minus press position, while held
} UI_Signal;

internal UI_Box*   ui_box(UI_BoxFlags flags, String8 string); // Default role
internal UI_Box*   ui_box_role(UI_Role role, UI_BoxFlags flags, String8 string);
internal UI_Box*   ui_boxf(UI_BoxFlags flags, char* fmt, ...);
internal UI_Signal ui_signal(UI_Box* box);

////////////////////////////////
//~ fp: Widgets
//
// Thin compositions of ui_box; the expected call sites for common cases.
// Labels and buttons size to their text; wrapped text fills the parent's
// width and grows downward. The begin/end pairs push/pop the parent stack --
// the UI_* macros wrap them in defer-loops.

internal void      ui_label(String8 string);
internal void      ui_labelf(char* fmt, ...);
internal void      ui_text_wrapped(String8 string);
internal UI_Signal ui_button(String8 string);
internal void      ui_spacer(UI_Size size); // along the parent's child axis
internal void      ui_tooltip(String8 string);

internal UI_Box* ui_row_begin(String8 string);    // children lay out along X
internal void    ui_row_end(void);
internal UI_Box* ui_column_begin(String8 string); // children lay out along Y
internal void    ui_column_end(void);
internal UI_Box* ui_panel_begin(String8 string);  // background+border+clip column
internal void    ui_panel_end(void);

#define UI_Parent(box)   DeferLoop(ui_push_parent(box), ui_pop_parent())
#define UI_Row(string)   DeferLoop(ui_row_begin(string), ui_row_end())
#define UI_Column(string) DeferLoop(ui_column_begin(string), ui_column_end())
#define UI_Panel(string) DeferLoop(ui_panel_begin(string), ui_panel_end())

#pragma once

#include "base/core.h"
#include "base/math.h"
#include "base/arena.h"
#include "base/strings.h"

////////////////////////////////
//~ fp: UI
//
// The core of an immediate mode UI. It uses the base layer alone.
//
// The input arrives as a UI_Input value. The result leaves as one array of
// UI_DrawCommand. The size of a text comes from the two functions that the
// caller gives to ui_init.
//
// The frame goes like this:
//
//   ui_frame_begin(frame_arena, input);
//   ... build the boxes and the widgets, and read ui_signal in any order ...
//   UI_DrawList list = ui_frame_end(); // solve the layout, and make the commands
//
// The boxes make a tree. Each frame builds that tree again, on the frame
// arena.
//
// The state that stays between two frames is in an internal table, and the key
// of a box addresses it. That state holds the animation of a hover and of a
// press, and the rects of the frame before. A read of an interaction during
// the build therefore tests against the layout of the frame before, which is
// one frame old.
//
// Each coordinate is in points, from the top left corner. Each color is a V4
// that holds RGBA from 0 to 1. Those are the rules of the draw layer, and this
// layer follows them without a dependency on it.

////////////////////////////////
//~ fp: Keys
//
// The key of a box names that box across the frames. A key is a hash of the
// string of the box, of the key of the parent box, and of the value at the top
// of the stack of seeds. Push a seed, such as the index of a loop or the id of
// a thing, to separate two boxes that a loop builds from one string.
//
// The rules for a string come from Dear ImGui and from RF:
//   "Label"          the hash of the whole string. The whole string shows.
//   "Label##suffix"  the hash of the whole string. "Label" alone shows.
//   "Label###suffix" the hash of "suffix" alone. "Label" alone shows. The
//                    text that shows can therefore change while the identity
//                    of the box stays.
//
// A string whose part for the hash is empty gives the key 0. Such a box exists
// for one frame, holds no state, and has no interaction.

typedef U64 UI_Key;

////////////////////////////////
//~ fp: Semantic Sizes
//
// Each box gives a preferred size on each axis, and ui_frame_end solves the
// tree.
//
// `strictness` is from 0 to 1. It is the part of the preferred size that the
// box keeps where the content of the parent is larger than the parent. A
// strictness of 1 keeps the whole size, and a strictness of 0 keeps none of
// it.

typedef U32 UI_Axis;
enum {
  UI_Axis_X,
  UI_Axis_Y,
  UI_Axis_COUNT,
};

typedef U32 UI_SizeKind;
enum {
  UI_SizeKind_ChildrenSum = 0, // The sum of the children along the axis of the
                               // children, and the largest child across that
                               // axis. It is the default (ZII).
  UI_SizeKind_Points,          // the value, in points
  UI_SizeKind_Text,            // The text of the box: its natural width, and a
                               // height of the number of its lines, after the
                               // wrap where the box wraps its text.
  UI_SizeKind_PctOfParent,     // The value, from 0 to 1, multiplied by the size
                               // of the content of the parent. It has no
                               // meaning where the parent takes its size from
                               // its children.
  UI_SizeKind_Grow,            // It starts at 0. It then takes the value
                               // divided by the sum of the values of each other
                               // child that grows, of the space that the parent
                               // has left along the axis of its children.
                               // Across that axis it takes the content of the
                               // parent in full.
};

typedef struct {
  UI_SizeKind kind;
  F32 value;
  F32 strictness;
} UI_Size;

internal UI_Size ui_size_points(F32 points, F32 strictness);
internal UI_Size ui_size_pct(F32 pct, F32 strictness);
internal UI_Size ui_size_text(F32 strictness);
internal UI_Size ui_size_children(F32 strictness);
internal UI_Size ui_size_grow(F32 weight);

////////////////////////////////
//~ fp: Boxes
//
// The box is the one kind of node. A widget is a set of flags and a style. The
// widget functions below make those sets, and a caller can make one directly.
//
// A box takes its style members from the stacks at the build. Those members
// stay writable until ui_frame_end reads them.

typedef U32 UI_BoxFlags;
enum {
  UI_BoxFlag_Clickable = (1 << 0), // The mouse can hit it. A hover and a press then animate.
  UI_BoxFlag_DrawBackground = (1 << 1),
  UI_BoxFlag_DrawBorder = (1 << 2),
  UI_BoxFlag_DrawText = (1 << 3),
  UI_BoxFlag_TextWrap = (1 << 4), // Wrap the text to the width that the solve
                                  // gives. Without this flag the text breaks at
                                  // a newline character alone.
  UI_BoxFlag_Clip = (1 << 5),     // clip the children and the text to the box
  UI_BoxFlag_Floating = (1 << 6), // The box is outside the layout. It stands at
                                  // floating_pos, relative to its parent.
};

typedef U32 UI_TextAlign;
enum {
  UI_TextAlign_Left = 0,
  UI_TextAlign_Center,
  UI_TextAlign_Right,
};

// The position of the children of a box across the axis of those children. The
// children of a row therefore align on the vertical axis, and the children of
// a column align on the horizontal axis.
typedef U32 UI_Align;
enum {
  UI_Align_Start = 0, // at the edge of the padding (ZII)
  UI_Align_Center,
  UI_Align_End,
};

typedef struct UI_BoxState UI_BoxState; // the state that stays between two frames, which this module alone reads

typedef struct UI_Box UI_Box;
struct UI_Box {
  //- The links of the tree. They exist for one frame.
  UI_Box* parent;
  UI_Box* first;
  UI_Box* last;
  UI_Box* next;
  UI_Box* prev;

  //- identity
  UI_Key key; // A key of 0 says that the box exists for one frame.
  UI_BoxFlags flags;
  String8 string; // the part that shows, on the frame arena
  U64 tags_key;   // It names the set of tags at the build. A color resolves
                  // against that set, and does so after the stack of tags
                  // returns to its earlier state.

  //- The style, which the box takes from the stacks at the build.
  UI_Size pref_size[UI_Axis_COUNT];
  UI_Axis child_axis;   // the children go along this axis
  UI_Align child_align; // the children align in this way across that axis
  U64 font;
  F32 font_size;
  V4 text_color;
  UI_TextAlign text_align;
  V4 background_color;
  V4 border_color;
  F32 border_thickness;
  F32 corner_radius;
  V2 padding;         // the space at each side between the box and its content
  F32 child_gap;      // the points between two children that follow each other
  V2 floating_pos;    // A floating box: the offset after the anchor below.
  V2 floating_anchor; // A floating box: a fraction from 0 to 1 on each axis. It
                      // names one point on the parent and the same point on the
                      // box. The box stands where those two points meet, and
                      // floating_pos then moves it. {0,0} puts the top left
                      // corner of the box on the top left corner of the parent,
                      // which is the default (ZII). {1,1} puts the bottom right
                      // corner on the bottom right corner. {0.5,0.5} puts the
                      // box at the center of the parent.

  //- ui_frame_end writes these members. Do not read them before that call.
  F32 fixed_size[UI_Axis_COUNT];
  Rect rect;
  String8* lines; // the lines that the frame draws, for a box with DrawText
  U64 line_count;

  UI_BoxState* state; // It is nil for a box that exists for one frame.
};

////////////////////////////////
//~ fp: Input
//
// The caller fills this struct in each frame, from the input system that it
// owns.
//
// The position of the mouse is in points. `pressed` and `released` are the two
// edges inside the frame, and `down` is the state that holds. `dt` is the
// seconds of the frame, which the animation reads. `window` is the size of the
// root box, in points.

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
// A font is a U64 handle. The two functions below read that handle, and this
// module does not.
//
// The measure function gives the size of the box around a run of text of one
// line. This module holds a cache of those sizes, whose key is the font, the
// size and the string, so it calls the function where the cache has no entry
// alone.

typedef struct {
  F32 ascent;       // from the top of the box to the baseline
  F32 descent;      // from the baseline to the bottom of the box
  F32 line_advance; // from one baseline to the next baseline
} UI_FontMetrics;

typedef V2 UI_MeasureTextFunc(void* user, U64 font, F32 size, String8 text);
typedef UI_FontMetrics UI_FontMetricsFunc(void* user, U64 font, F32 size);

////////////////////////////////
//~ fp: Tags / Theme
//
// A color resolves in the way that a selector of CSS does. The theme holds a
// color and nothing more: each size, each padding and each radius is a value in
// the code.
//
// A call site pushes a tag, which is a string that names what the code builds
// there, such as "button" or "accent". A color that a box does not take from a
// stack resolves against the theme.
//
// The theme is a list of patterns. Each pattern holds a list of tags and a
// color. A pattern applies where each of its tags is in the set of the tags of
// the box together with the name of the color, and where that name is among
// its tags. The names of the colors are "background", "text" and "border",
// with "hover" and "active" for a box that a person can click. The pattern
// with the most tags wins. Where no pattern applies the color is 0, which
// gives an appearance that a person sees at once.
//
// The theme is data. The caller installs it in full, and this module holds a
// reference to it. The caller must therefore keep the memory of the patterns,
// which a load from a file at the start of the program usually gives.
//
// This module holds a cache of the results, whose key is the hash of the set of
// tags. It therefore reads the patterns one time for each pair of a set of
// tags and a name, and not one time for each box.

typedef struct {
  String8* tags;
  U64 count;
  V4 color;
} UI_ThemePattern;

typedef struct {
  UI_ThemePattern* patterns;
  U64 count;
} UI_Theme;

internal UI_Theme ui_theme_load(Arena* arena, String8 path);
internal void ui_set_theme(UI_Theme theme);
internal void ui_push_tag(String8 tag);
internal void ui_pop_tag(void);
internal V4 ui_color_from_name(String8 name); // resolve the name against the tags that are on the stack now

////////////////////////////////
//~ fp: Draw Commands
//
// The result of ui_frame_end is one array on the frame arena, in the order to
// draw.
//
// A Rect command holds the same values as a rect of the draw layer. A Text
// command holds one line, which contains no newline character, and its `pos`
// is the top left corner of the box around that line. A second ClipPush inside
// the first gives the intersection of the two, and each ClipPush has one
// ClipPop.

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
  F32 border_thickness; // A value of 0 fills the rect.
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
  B32 mouse_over_ui; // The mouse is inside a box that a person can click, or
                     // inside a box that draws a background. It is also true
                     // while a press that started on such a box continues.
} UI_DrawList;

////////////////////////////////
//~ fp: Frame Lifecycle

internal void ui_init(UI_MeasureTextFunc* measure, UI_FontMetricsFunc* metrics, void* user);
internal void ui_frame_begin(Arena* frame_arena, UI_Input input);
internal UI_DrawList ui_frame_end(void);

internal UI_Box* ui_root(void); // the root box of this frame, at the size of the window
internal V2 ui_mouse(void);     // the mouse of this frame, in points

////////////////////////////////
//~ fp: Style Stacks
//
// ui_frame_begin sets each stack to a default that suits a struct of zeros. A
// box takes the value at the top of each stack at its build. Each push needs a
// pop inside the same frame, which an assert tests.
//
// One row of UI_STYLE_STACK_LIST is one stack: its name, the type of its
// values, and its default at the start of a frame. The row generates the
// member of the state, the push and pop functions, and the reset at the frame
// begin, so the four places cannot disagree. The stack of the parent and the
// stacks of the tags carry more than this shape, so they stay written out.

#define UI_STYLE_STACK_LIST                                  \
  X(seed,             U64,          0)                       \
  X(pref_width,       UI_Size,      ui_size_children(0))     \
  X(pref_height,      UI_Size,      ui_size_children(0))     \
  X(font,             U64,          0)                       \
  X(font_size,        F32,          16.0f)                   \
  X(text_color,       V4,           ((V4){0}))               \
  X(text_align,       UI_TextAlign, UI_TextAlign_Left)       \
  X(background_color, V4,           ((V4){0}))               \
  X(border_color,     V4,           ((V4){0}))               \
  X(border_thickness, F32,          1.0f)                    \
  X(corner_radius,    F32,          0.0f)                    \
  X(child_axis,       UI_Axis,      UI_Axis_Y)               \
  X(child_align,      UI_Align,     UI_Align_Start)          \
  X(padding,          V2,           ((V2){0, 0}))            \
  X(child_gap,        F32,          0.0f)

internal void ui_push_parent(UI_Box* box);
internal void ui_pop_parent(void);
// ui_push_tag and ui_pop_tag are in the Tags / Theme section above.

#define X(name, type, default_value)      \
  internal void ui_push_##name(type value); \
  internal void ui_pop_##name(void);
UI_STYLE_STACK_LIST
#undef X

////////////////////////////////
//~ fp: Building / Signals
//
// ui_box adds a box under the current parent, and gives that box back, so that
// the caller can write its members.
//
// ui_signal reads the interaction of a box against the layout of the frame
// before. `hovered` follows the box that a person can click and that is above
// each other such box under the mouse. `clicked` says that the left button
// came up over the box on which it went down.

typedef struct {
  UI_Box* box; // the box of this signal, so that the caller can write its members
  B8 hovered;
  B8 pressed;  // the left button went down on the box in this frame
  B8 down;     // the left button stays down since that press
  B8 released; // the left button came up in this frame, at any position of the mouse
  B8 clicked;
  B8 right_clicked;
  V2 drag_delta; // the mouse minus the position of the press, while the button is down
} UI_Signal;

internal UI_Box* ui_box(UI_BoxFlags flags, String8 string);
internal UI_Box* ui_boxf(UI_BoxFlags flags, char* fmt, ...);
internal UI_Signal ui_signal(UI_Box* box);

////////////////////////////////
//~ fp: Widgets
//
// Each widget is a small set of calls to ui_box. Use these functions for the
// usual cases.
//
// A button, a panel and a tooltip push their name as a tag around their box, so
// that the theme can address them. The tag of a panel covers its children. The
// sizes of those widgets are values in this file, and a caller can change them
// on the box that the function gives back.
//
// A label and a button take the size of their text. A wrapped text takes the
// width of its parent, and grows toward the bottom.
//
// Each pair of a begin and an end pushes the parent stack and pops it. Each
// UI_ macro below puts such a pair in a loop that runs one time.

internal void ui_label(String8 string);
internal void ui_labelf(char* fmt, ...);
internal void ui_text_wrapped(String8 string);
internal UI_Signal ui_button(String8 string);
internal void ui_spacer(UI_Size size); // along the axis of the children of the parent
internal void ui_tooltip(String8 string);

internal UI_Box* ui_row_begin(String8 string); // the children go along X
internal void ui_row_end(void);
internal UI_Box* ui_column_begin(String8 string); // the children go along Y
internal void ui_column_end(void);
internal UI_Box* ui_panel_begin(String8 string); // a column with a background, a border and a clip
internal void ui_panel_end(void);

#define UI_Tag(string)    DeferLoop(ui_push_tag(string), ui_pop_tag())
#define UI_Parent(box)    DeferLoop(ui_push_parent(box), ui_pop_parent())
#define UI_Row(string)    DeferLoop(ui_row_begin(string), ui_row_end())
#define UI_Column(string) DeferLoop(ui_column_begin(string), ui_column_end())
#define UI_Panel(string)  DeferLoop(ui_panel_begin(string), ui_panel_end())

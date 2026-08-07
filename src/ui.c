#include "base/core.h"
#include "base/math.h"
#include "base/arena.h"
#include "base/tctx.h"
#include "base/strings.h"
#include "ui.h"
#include "tabula.h"

////////////////////////////////
//~ fp: UI State
//
// One module-internal global, like the gfx modules. The frame arena and
// everything on it (boxes, lines, draw commands) live for one frame; the
// persistent arena holds the box-state table and the text cache for the
// module's whole life. Box states not touched for a frame are recycled
// through a free list at the next ui_frame_begin.

#define UI_STACK_CAP         64
#define UI_STATE_BUCKETS     256  // power of two
#define UI_TEXT_CACHE_SLOTS  4096 // power of two
#define UI_TAG_SET_BUCKETS   64   // power of two
#define UI_COLOR_CACHE_SLOTS 1024 // power of two

struct UI_BoxState {
  UI_BoxState* hash_next; // bucket chain, or free-list link when recycled
  UI_Key key;
  U64 last_frame_touched;
  Rect rect; // final rect as of the last frame the box was built
  B8 rect_valid;
  F32 hot_t;    // 0..1, chases "is the hot box"
  F32 active_t; // 0..1, chases "is the active box"
};

// direct-mapped measurement cache; a colliding key overwrites the slot
typedef struct {
  U64 key; // 0 = empty
  V2 dim;
} UI_TextCacheSlot;

// tag-set key -> the pushed-tag strings it stands for, recorded at
// ui_push_tag on the persistent arena so any later resolution can decode a
// box's tags_key back into strings for pattern matching
typedef struct UI_TagSetNode UI_TagSetNode;
struct UI_TagSetNode {
  UI_TagSetNode* next;
  U64 key;
  String8* tags;
  U64 count;
};

// direct-mapped (tag set, color name) -> color cache; a colliding key
// overwrites the slot. Cleared whole by ui_set_theme.
typedef struct {
  U64 key; // 0 = empty
  V4 color;
} UI_ColorCacheSlot;

typedef struct {
  //- init-time
  Arena* persist;
  UI_MeasureTextFunc* measure;
  UI_FontMetricsFunc* metrics;
  void* user;
  UI_Theme theme;                 // caller-owned pattern memory, installed by ui_set_theme
  UI_TextCacheSlot* text_cache;   // [UI_TEXT_CACHE_SLOTS]
  UI_ColorCacheSlot* color_cache; // [UI_COLOR_CACHE_SLOTS]
  UI_TagSetNode* tag_set_buckets[UI_TAG_SET_BUCKETS];
  UI_BoxState* state_buckets[UI_STATE_BUCKETS];
  UI_BoxState* state_free;

  //- frame
  Arena* arena; // the caller's frame arena, held between begin and end
  UI_Input input;
  U64 frame;
  UI_Box* root;
  U64 box_count;
  U64 line_total; // drawn text lines this frame, for the command cap
  UI_DrawCommand* cmds;
  U64 cmd_count;
  U64 cmd_cap;

  //- interaction
  UI_Key hot_key;      // topmost clickable box under the mouse, per the
                       // previous frame; pinned to active_key while held
  UI_Key hot_key_next; // being computed during emission, for next frame
  UI_Key active_key;   // box the left button went down on, until release
  V2 press_mouse;      // mouse position at that press
  B32 mouse_over;      // computed during emission

  //- style stacks; bottoms are the per-frame defaults
  struct {
    UI_Box* items[UI_STACK_CAP];
    U64 top;
  } parent_stack;
  struct {
    U64 items[UI_STACK_CAP];
    U64 top;
  } seed_stack;
  struct {
    String8 items[UI_STACK_CAP];
    U64 top;
  } tag_stack;
  struct { // running hash of tag_stack's contents, entry for entry
    U64 items[UI_STACK_CAP];
    U64 top;
  } tag_key_stack;
  struct {
    UI_Size items[UI_STACK_CAP];
    U64 top;
  } pref_width_stack;
  struct {
    UI_Size items[UI_STACK_CAP];
    U64 top;
  } pref_height_stack;
  struct {
    U64 items[UI_STACK_CAP];
    U64 top;
  } font_stack;
  struct {
    F32 items[UI_STACK_CAP];
    U64 top;
  } font_size_stack;
  struct {
    V4 items[UI_STACK_CAP];
    U64 top;
  } text_color_stack;
  struct {
    UI_TextAlign items[UI_STACK_CAP];
    U64 top;
  } text_align_stack;
  struct {
    V4 items[UI_STACK_CAP];
    U64 top;
  } background_color_stack;
  struct {
    V4 items[UI_STACK_CAP];
    U64 top;
  } border_color_stack;
  struct {
    F32 items[UI_STACK_CAP];
    U64 top;
  } border_thickness_stack;
  struct {
    F32 items[UI_STACK_CAP];
    U64 top;
  } corner_radius_stack;
  struct {
    UI_Axis items[UI_STACK_CAP];
    U64 top;
  } child_axis_stack;
  struct {
    UI_Align items[UI_STACK_CAP];
    U64 top;
  } child_align_stack;
  struct {
    V2 items[UI_STACK_CAP];
    U64 top;
  } padding_stack;
  struct {
    F32 items[UI_STACK_CAP];
    U64 top;
  } child_gap_stack;
} UI_State;

global UI_State ui_state;

#define ui__push(stack, v) Stmnt(Assert(ui_state.stack.top < UI_STACK_CAP);      \
                                 ui_state.stack.items[ui_state.stack.top] = (v); \
                                 ui_state.stack.top += 1;)
#define ui__pop(stack)    Stmnt(Assert(ui_state.stack.top > 1); ui_state.stack.top -= 1;)
#define ui__top(stack)    (ui_state.stack.items[ui_state.stack.top - 1])
#define ui__pushed(stack) (ui_state.stack.top > 1) // above its default depth

////////////////////////////////
//~ fp: Small Helpers

internal F32 ui__v2_axis(V2 v, UI_Axis axis) {
  return axis == UI_Axis_X ? v.x : v.y;
}

// min inclusive, max exclusive
internal B32 ui__rect_contains(Rect r, V2 p) {
  return p.x >= r.min.x && p.x < r.max.x && p.y >= r.min.y && p.y < r.max.y;
}

//- fp: fnv-1a over the bytes, then a splitmix64 finalizer so sequential
//  inputs land far apart
internal U64 ui__hash_str8(U64 seed, String8 s) {
  U64 h = seed ^ 0xcbf29ce484222325ull;
  for(U64 i = 0; i < s.size; i += 1) {
    h = (h ^ s.str[i]) * 0x100000001b3ull;
  }
  return h;
}

internal U64 ui__mix64(U64 h) {
  h ^= h >> 30;
  h *= 0xbf58476d1ce4e5b9ull;
  h ^= h >> 27;
  h *= 0x94d049bb133111ebull;
  h ^= h >> 31;
  return h;
}

//- fp: the "##"/"###" split (see the Keys section in ui.h)
internal String8 ui__display_part(String8 s) {
  return str8_prefix(s, str8_find_substr(s, str8_lit("##"), 0, 0));
}

internal String8 ui__hash_part(String8 s) {
  U64 pos = str8_find_substr(s, str8_lit("###"), 0, 0);
  if(pos < s.size) {
    return str8_skip(s, pos + 3);
  }
  return s;
}

////////////////////////////////
//~ fp: Text Measurement
//
// All measurement goes through the cache; the callback runs on misses only.
// Keyed by (font, size bits, string bytes).

internal V2 ui__measure_text(U64 font, F32 size, String8 text) {
  UI_State* s = &ui_state;
  if(s->measure == 0) {
    return (V2){0};
  }
  U32 size_bits = 0;
  MemoryCopy(&size_bits, &size, sizeof(size_bits));
  U64 key = ui__mix64(ui__hash_str8(font ^ ((U64)size_bits << 32), text));
  if(key == 0) {
    key = 1;
  }
  UI_TextCacheSlot* slot = &s->text_cache[key & (UI_TEXT_CACHE_SLOTS - 1)];
  if(slot->key != key) {
    slot->key = key;
    slot->dim = s->measure(s->user, font, size, text);
  }
  return slot->dim;
}

internal UI_FontMetrics ui__font_metrics(U64 font, F32 size) {
  UI_State* s = &ui_state;
  if(s->metrics == 0) {
    return (UI_FontMetrics){0};
  }
  return s->metrics(s->user, font, size);
}

////////////////////////////////
//~ fp: Persistent Box State

internal UI_BoxState* ui__state_for_key(UI_Key key) {
  UI_State* s = &ui_state;
  UI_BoxState** bucket = &s->state_buckets[key & (UI_STATE_BUCKETS - 1)];
  UI_BoxState* state = *bucket;
  for(; state != 0 && state->key != key; state = state->hash_next) {}
  if(state == 0) {
    if(s->state_free != 0) {
      state = s->state_free;
      s->state_free = state->hash_next;
    } else {
      state = push_array_no_zero(s->persist, UI_BoxState, 1);
    }
    MemoryZeroStruct(state);
    state->key = key;
    state->hash_next = *bucket;
    *bucket = state;
  }
  state->last_frame_touched = s->frame;
  return state;
}

////////////////////////////////
//~ fp: Sizes

internal UI_Size ui_size_points(F32 points, F32 strictness) {
  return (UI_Size){UI_SizeKind_Points, points, strictness};
}

internal UI_Size ui_size_pct(F32 pct, F32 strictness) {
  return (UI_Size){UI_SizeKind_PctOfParent, pct, strictness};
}

internal UI_Size ui_size_text(F32 strictness) {
  return (UI_Size){UI_SizeKind_Text, 0, strictness};
}

internal UI_Size ui_size_children(F32 strictness) {
  return (UI_Size){UI_SizeKind_ChildrenSum, 0, strictness};
}

internal UI_Size ui_size_grow(F32 weight) {
  return (UI_Size){UI_SizeKind_Grow, weight, 0};
}

////////////////////////////////
//~ fp: Style Stacks

internal void ui_push_parent(UI_Box* box) {
  ui__push(parent_stack, box);
}
internal void ui_pop_parent(void) {
  ui__pop(parent_stack);
}
internal void ui_push_seed(U64 seed) {
  ui__push(seed_stack, seed);
}
internal void ui_pop_seed(void) {
  ui__pop(seed_stack);
}
internal void ui_push_pref_width(UI_Size size) {
  ui__push(pref_width_stack, size);
}
internal void ui_pop_pref_width(void) {
  ui__pop(pref_width_stack);
}
internal void ui_push_pref_height(UI_Size size) {
  ui__push(pref_height_stack, size);
}
internal void ui_pop_pref_height(void) {
  ui__pop(pref_height_stack);
}
internal void ui_push_font(U64 font) {
  ui__push(font_stack, font);
}
internal void ui_pop_font(void) {
  ui__pop(font_stack);
}
internal void ui_push_font_size(F32 size) {
  ui__push(font_size_stack, size);
}
internal void ui_pop_font_size(void) {
  ui__pop(font_size_stack);
}
internal void ui_push_text_color(V4 color) {
  ui__push(text_color_stack, color);
}
internal void ui_pop_text_color(void) {
  ui__pop(text_color_stack);
}
internal void ui_push_text_align(UI_TextAlign align) {
  ui__push(text_align_stack, align);
}
internal void ui_pop_text_align(void) {
  ui__pop(text_align_stack);
}
internal void ui_push_background_color(V4 color) {
  ui__push(background_color_stack, color);
}
internal void ui_pop_background_color(void) {
  ui__pop(background_color_stack);
}
internal void ui_push_border_color(V4 color) {
  ui__push(border_color_stack, color);
}
internal void ui_pop_border_color(void) {
  ui__pop(border_color_stack);
}
internal void ui_push_border_thickness(F32 thickness) {
  ui__push(border_thickness_stack, thickness);
}
internal void ui_pop_border_thickness(void) {
  ui__pop(border_thickness_stack);
}
internal void ui_push_corner_radius(F32 radius) {
  ui__push(corner_radius_stack, radius);
}
internal void ui_pop_corner_radius(void) {
  ui__pop(corner_radius_stack);
}
internal void ui_push_child_axis(UI_Axis axis) {
  ui__push(child_axis_stack, axis);
}
internal void ui_pop_child_axis(void) {
  ui__pop(child_axis_stack);
}
internal void ui_push_child_align(UI_Align align) {
  ui__push(child_align_stack, align);
}
internal void ui_pop_child_align(void) {
  ui__pop(child_align_stack);
}
internal void ui_push_padding(V2 padding) {
  ui__push(padding_stack, padding);
}
internal void ui_pop_padding(void) {
  ui__pop(padding_stack);
}
internal void ui_push_child_gap(F32 gap) {
  ui__push(child_gap_stack, gap);
}
internal void ui_pop_child_gap(void) {
  ui__pop(child_gap_stack);
}

////////////////////////////////
//~ fp: Tags / Theme

internal void ui_set_theme(UI_Theme theme) {
  ui_state.theme = theme;
  MemoryZero(ui_state.color_cache, sizeof(UI_ColorCacheSlot) * UI_COLOR_CACHE_SLOTS);
}

// data/ui.tabula -> UI_Theme: the nesting is the selector. The path of keys
// down to each color leaf becomes a pattern's tag list -- style.button.background
// reads as ["button" "background"] -- so no key name is known here; the
// vocabulary lives in the file and at the call sites that push the tags.
// Leaves must be [r g b (a)] colors (metrics are not themed, asserted); a
// missing pattern resolves to zero in the ui module, an obviously unstyled
// look, never a quietly substituted default. The theme's memory lives on
// `arena`, which must outlive the ui module's use of it.
typedef struct UI_ThemeNode UI_ThemeNode;
struct UI_ThemeNode {
  UI_ThemeNode* next;
  UI_ThemePattern pattern;
};

internal void ui__theme_walk(Arena* arena, Arena* scratch, TB_Value* object,
                             String8* path, U64 depth,
                             UI_ThemeNode** first, U64* count) {
  for(TB_Node* node = object->first_member; node != 0; node = node->next) {
    Assert(depth < 8); // path[]'s capacity
    path[depth] = node->key;
    if(node->value.kind == TB_ValueKind_Object) {
      ui__theme_walk(arena, scratch, &node->value, path, depth + 1, first, count);
    } else {
      TB_Value* list = &node->value;
      Assert(list->kind == TB_ValueKind_List && list->count >= 3);
      V4 color = {0, 0, 0, 1};
      F32* comps[4] = {&color.x, &color.y, &color.z, &color.w};
      U64 ci = 0;
      for(TB_Value* el = list->first; el != 0 && ci < 4; el = el->next, ci += 1) {
        *comps[ci] = tb_num_from_value(el, 0);
      }
      UI_ThemeNode* n = push_array(scratch, UI_ThemeNode, 1);
      n->pattern.count = depth + 1;
      n->pattern.tags = push_array(arena, String8, n->pattern.count);
      for(U64 i = 0; i < n->pattern.count; i += 1) {
        n->pattern.tags[i] = push_str8_copy(arena, path[i]);
      }
      n->pattern.color = color;
      n->next = *first;
      *first = n;
      *count += 1;
    }
  }
}

internal UI_Theme ui_theme_load(Arena* arena, String8 path) {
  UI_Theme theme = {0};
  ArenaTemp scratch = arena_get_scratch(&arena, 1);
  TB_Value* root = tb_parse_file_and_report(scratch.arena, path);
  TB_Value* style = tb_get(root, str8_lit("style"));
  String8 tag_path[8] = {0};
  UI_ThemeNode* first = 0;
  ui__theme_walk(arena, scratch.arena, style, tag_path, 0, &first, &theme.count);
  theme.patterns = push_array(arena, UI_ThemePattern, theme.count);
  U64 idx = 0;
  for(UI_ThemeNode* n = first; n != 0; n = n->next, idx += 1) {
    theme.patterns[idx] = n->pattern;
  }
  arena_release_scratch(scratch);
  return theme;
}


// pushing chains the running hash; the first sight of a tag set copies its
// strings to the persistent arena, so a key decodes back to strings for as
// long as the module lives
internal void ui_push_tag(String8 tag) {
  UI_State* s = &ui_state;
  U64 key = ui__mix64(ui__hash_str8(ui__top(tag_key_stack), tag));
  if(key == 0) { key = 1; }
  ui__push(tag_stack, tag);
  ui__push(tag_key_stack, key);
  U64 bucket = key & (UI_TAG_SET_BUCKETS - 1);
  UI_TagSetNode* node = s->tag_set_buckets[bucket];
  for(; node != 0 && node->key != key; node = node->next) {}
  if(node == 0) {
    node = push_array(s->persist, UI_TagSetNode, 1);
    node->key = key;
    node->count = s->tag_stack.top - 1; // the entries above the empty bottom
    node->tags = push_array(s->persist, String8, node->count);
    for(U64 i = 0; i < node->count; i += 1) {
      node->tags[i] = push_str8_copy(s->persist, s->tag_stack.items[i + 1]);
    }
    node->next = s->tag_set_buckets[bucket];
    s->tag_set_buckets[bucket] = node;
  }
}

internal void ui_pop_tag(void) {
  ui__pop(tag_stack);
  ui__pop(tag_key_stack);
}

// most specific matching pattern wins: every pattern tag present in the
// set + name, the name itself among them, highest tag count first
internal V4 ui__color_from_key_name(U64 tags_key, String8 name) {
  UI_State* s = &ui_state;
  U64 final_key = ui__mix64(ui__hash_str8(tags_key, name));
  if(final_key == 0) { final_key = 1; }
  UI_ColorCacheSlot* slot = &s->color_cache[final_key & (UI_COLOR_CACHE_SLOTS - 1)];
  if(slot->key != final_key) {
    String8* tags = 0;
    U64 tag_count = 0;
    for(UI_TagSetNode* n = s->tag_set_buckets[tags_key & (UI_TAG_SET_BUCKETS - 1)];
        n != 0; n = n->next) {
      if(n->key == tags_key) {
        tags = n->tags;
        tag_count = n->count;
        break;
      }
    }
    V4 color = {0};
    U64 best = 0;
    for(U64 i = 0; i < s->theme.count; i += 1) {
      UI_ThemePattern* p = &s->theme.patterns[i];
      B32 has_name = 0;
      B32 all_present = 1;
      for(U64 t = 0; t < p->count && all_present; t += 1) {
        B32 present = str8_match(p->tags[t], name, 0);
        has_name |= present;
        for(U64 k = 0; !present && k < tag_count; k += 1) {
          present = str8_match(p->tags[t], tags[k], 0);
        }
        all_present = present;
      }
      if(has_name && all_present && p->count > best) {
        best = p->count;
        color = p->color;
      }
    }
    slot->key = final_key;
    slot->color = color;
  }
  return slot->color;
}

internal V4 ui_color_from_name(String8 name) {
  return ui__color_from_key_name(ui__top(tag_key_stack), name);
}

////////////////////////////////
//~ fp: Frame Lifecycle

internal void ui_init(UI_MeasureTextFunc* measure, UI_FontMetricsFunc* metrics, void* user) {
  MemoryZeroStruct(&ui_state);
  ui_state.persist = arena_alloc();
  ui_state.measure = measure;
  ui_state.metrics = metrics;
  ui_state.user = user;
  ui_state.text_cache = push_array(ui_state.persist, UI_TextCacheSlot, UI_TEXT_CACHE_SLOTS);
  ui_state.color_cache = push_array(ui_state.persist, UI_ColorCacheSlot, UI_COLOR_CACHE_SLOTS);
}

internal UI_Box* ui_root(void) {
  return ui_state.root;
}

internal V2 ui_mouse(void) {
  return ui_state.input.mouse;
}

internal void ui_frame_begin(Arena* frame_arena, UI_Input input) {
  UI_State* s = &ui_state;
  Assert(s->persist != 0); // ui_init must run first
  s->arena = frame_arena;
  s->input = input;
  s->frame += 1;
  s->line_total = 0;
  s->mouse_over = 0;
  s->cmds = 0;
  s->cmd_count = 0;
  s->cmd_cap = 0;

  //- hover follows the previous frame's topmost hit; a held press pins it
  s->hot_key = (s->active_key != 0) ? s->active_key : s->hot_key_next;
  s->hot_key_next = 0;

  //- recycle state not touched last frame
  for(U64 b = 0; b < UI_STATE_BUCKETS; b += 1) {
    for(UI_BoxState** at = &s->state_buckets[b]; *at != 0;) {
      UI_BoxState* state = *at;
      if(state->last_frame_touched + 1 < s->frame) {
        *at = state->hash_next;
        state->hash_next = s->state_free;
        s->state_free = state;
      } else {
        at = &state->hash_next;
      }
    }
  }

  //- window-sized root; transient (key 0), outside every stack
  UI_Box* root = push_array(s->arena, UI_Box, 1);
  root->pref_size[UI_Axis_X] = ui_size_points(input.window.x, 1);
  root->pref_size[UI_Axis_Y] = ui_size_points(input.window.y, 1);
  root->child_axis = UI_Axis_Y;
  s->root = root;
  s->box_count = 1;

  //- reset the stacks to their defaults; the color stacks' bottoms are
  //  never read -- a color stack left unpushed resolves from the theme
  s->parent_stack.top = 0;
  s->seed_stack.top = 0;
  s->tag_stack.top = 0;
  s->tag_key_stack.top = 0;
  s->pref_width_stack.top = 0;
  s->pref_height_stack.top = 0;
  s->font_stack.top = 0;
  s->font_size_stack.top = 0;
  s->text_color_stack.top = 0;
  s->text_align_stack.top = 0;
  s->background_color_stack.top = 0;
  s->border_color_stack.top = 0;
  s->border_thickness_stack.top = 0;
  s->corner_radius_stack.top = 0;
  s->child_axis_stack.top = 0;
  s->child_align_stack.top = 0;
  s->padding_stack.top = 0;
  s->child_gap_stack.top = 0;
  ui__push(parent_stack, root);
  ui__push(seed_stack, 0);
  ui__push(tag_stack, str8_lit(""));
  ui__push(tag_key_stack, 0);
  ui__push(pref_width_stack, ui_size_children(0));
  ui__push(pref_height_stack, ui_size_children(0));
  ui__push(font_stack, 0);
  ui__push(font_size_stack, 16.0f);
  ui__push(text_color_stack, ((V4){0}));
  ui__push(text_align_stack, UI_TextAlign_Left);
  ui__push(background_color_stack, ((V4){0}));
  ui__push(border_color_stack, ((V4){0}));
  ui__push(border_thickness_stack, 1.0f);
  ui__push(corner_radius_stack, 0.0f);
  ui__push(child_axis_stack, UI_Axis_Y);
  ui__push(child_align_stack, UI_Align_Start);
  ui__push(padding_stack, ((V2){0, 0}));
  ui__push(child_gap_stack, 0.0f);
}

////////////////////////////////
//~ fp: Building / Signals

internal UI_Box* ui_box(UI_BoxFlags flags, String8 string) {
  UI_State* s = &ui_state;
  UI_Box* parent = ui__top(parent_stack);
  UI_Box* box = push_array(s->arena, UI_Box, 1);
  s->box_count += 1;

  String8 hash_part = ui__hash_part(string);
  if(hash_part.size > 0) {
    U64 seed = parent->key ^ ui__top(seed_stack);
    box->key = ui__mix64(ui__hash_str8(seed, hash_part));
    if(box->key == 0) {
      box->key = 1;
    }
    box->state = ui__state_for_key(box->key);
  }
  box->flags = flags;
  box->string = push_str8_copy(s->arena, ui__display_part(string));

  box->pref_size[UI_Axis_X] = ui__top(pref_width_stack);
  box->pref_size[UI_Axis_Y] = ui__top(pref_height_stack);
  box->child_axis = ui__top(child_axis_stack);
  box->child_align = ui__top(child_align_stack);
  box->font = ui__top(font_stack);
  box->text_align = ui__top(text_align_stack);
  box->font_size = ui__top(font_size_stack);
  box->border_thickness = ui__top(border_thickness_stack);
  box->corner_radius = ui__top(corner_radius_stack);
  box->padding = ui__top(padding_stack);
  box->child_gap = ui__top(child_gap_stack);

  //- colors: a pushed stack trumps the theme
  box->tags_key = ui__top(tag_key_stack);
  box->text_color = ui__pushed(text_color_stack) ? ui__top(text_color_stack) : ui__color_from_key_name(box->tags_key, str8_lit("text"));
  box->background_color = ui__pushed(background_color_stack) ? ui__top(background_color_stack) : ui__color_from_key_name(box->tags_key, str8_lit("background"));
  box->border_color = ui__pushed(border_color_stack) ? ui__top(border_color_stack) : ui__color_from_key_name(box->tags_key, str8_lit("border"));

  box->parent = parent;
  DLLPushBack(parent->first, parent->last, box);
  return box;
}

internal UI_Box* ui_boxf(UI_BoxFlags flags, char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  String8 string = push_str8fv(ui_state.arena, fmt, args);
  va_end(args);
  return ui_box(flags, string);
}

internal UI_Signal ui_signal(UI_Box* box) {
  UI_State* s = &ui_state;
  UI_Signal sig = {0};
  sig.box = box;
  if(box == 0 || box->key == 0) {
    return sig;
  }
  UI_Input* in = &s->input;
  B32 hot = (s->hot_key == box->key);
  sig.hovered = (B8)hot;
  if((box->flags & UI_BoxFlag_Clickable) && hot && in->pressed[UI_MouseButton_Left]) {
    s->active_key = box->key;
    s->press_mouse = in->mouse;
    sig.pressed = 1;
  }
  if(s->active_key == box->key) {
    sig.down = in->down[UI_MouseButton_Left];
    sig.released = in->released[UI_MouseButton_Left];
    sig.clicked = (B8)(sig.released && box->state->rect_valid &&
                       ui__rect_contains(box->state->rect, in->mouse));
    sig.drag_delta = v2_sub(in->mouse, s->press_mouse);
  }
  if(hot && in->pressed[UI_MouseButton_Right]) {
    sig.right_clicked = 1;
  }
  return sig;
}

////////////////////////////////
//~ fp: Layout
//
// Axis-major: every pass runs for X, then text lines break against the final
// widths, then every pass runs for Y (so wrapped heights see solved widths),
// then one position pass places both axes. Within an axis: standalone kinds,
// then percent-of-parent preorder, then children-sum postorder, then grow
// preorder, then violation resolution preorder. Grow runs before violations:
// a parent has leftover space or overflow along an axis, never both, so the
// two passes act on disjoint parents. Floating boxes take no part in sums,
// grow, violations, or the layout cursor.

internal void ui__layout_standalone(UI_Box* box, UI_Axis axis) {
  UI_Size size = box->pref_size[axis];
  F32 pad2 = 2 * ui__v2_axis(box->padding, axis);
  switch(size.kind) {
    case UI_SizeKind_Points: {
      box->fixed_size[axis] = size.value;
    } break;
    case UI_SizeKind_Grow: {
      box->fixed_size[axis] = 0;
    } break;
    case UI_SizeKind_Text: {
      if(axis == UI_Axis_X) {
        // natural width: the widest '\n'-separated line
        F32 w = 0;
        String8 s = box->string;
        U64 start = 0;
        for(U64 i = 0; i <= s.size; i += 1) {
          if(i == s.size || s.str[i] == '\n') {
            w = Max(w, ui__measure_text(box->font, box->font_size, str8_substr(s, start, i)).x);
            start = i + 1;
          }
        }
        box->fixed_size[axis] = w + pad2;
      } else {
        UI_FontMetrics m = ui__font_metrics(box->font, box->font_size);
        F32 h = 0;
        if(box->line_count > 0) {
          h = (F32)(box->line_count - 1) * m.line_advance + m.ascent + m.descent;
        }
        box->fixed_size[axis] = h + pad2;
      }
    } break;
    default: break; // ChildrenSum / PctOfParent resolve in later passes
  }
  for(UI_Box* child = box->first; child != 0; child = child->next) {
    ui__layout_standalone(child, axis);
  }
}

internal void ui__layout_pct(UI_Box* box, UI_Axis axis) {
  if(box->pref_size[axis].kind == UI_SizeKind_PctOfParent && box->parent != 0) {
    F32 content = box->parent->fixed_size[axis] - 2 * ui__v2_axis(box->parent->padding, axis);
    box->fixed_size[axis] = ClampBot(content, 0) * box->pref_size[axis].value;
  }
  for(UI_Box* child = box->first; child != 0; child = child->next) {
    ui__layout_pct(child, axis);
  }
}

internal void ui__layout_sum(UI_Box* box, UI_Axis axis) {
  for(UI_Box* child = box->first; child != 0; child = child->next) {
    ui__layout_sum(child, axis);
  }
  if(box->pref_size[axis].kind == UI_SizeKind_ChildrenSum) {
    F32 total = 0;
    U64 n = 0;
    for(UI_Box* child = box->first; child != 0; child = child->next) {
      if(child->flags & UI_BoxFlag_Floating) { continue; }
      if(axis == box->child_axis) {
        total += child->fixed_size[axis];
      } else {
        total = Max(total, child->fixed_size[axis]);
      }
      n += 1;
    }
    if(axis == box->child_axis && n > 1) {
      total += box->child_gap * (F32)(n - 1);
    }
    box->fixed_size[axis] = total + 2 * ui__v2_axis(box->padding, axis);
  }
}

internal void ui__layout_grow(UI_Box* box, UI_Axis axis) {
  F32 content = ClampBot(box->fixed_size[axis] - 2 * ui__v2_axis(box->padding, axis), 0);
  if(axis == box->child_axis) {
    //- weighted split of the parent's leftover among its grow children
    F32 total = 0;
    F32 weights = 0;
    U64 n = 0;
    for(UI_Box* child = box->first; child != 0; child = child->next) {
      if(child->flags & UI_BoxFlag_Floating) { continue; }
      total += child->fixed_size[axis];
      if(child->pref_size[axis].kind == UI_SizeKind_Grow) {
        weights += child->pref_size[axis].value;
      }
      n += 1;
    }
    if(n > 1) {
      total += box->child_gap * (F32)(n - 1);
    }
    F32 leftover = content - total;
    if(leftover > 0 && weights > 0) {
      for(UI_Box* child = box->first; child != 0; child = child->next) {
        if(child->flags & UI_BoxFlag_Floating) { continue; }
        if(child->pref_size[axis].kind == UI_SizeKind_Grow) {
          child->fixed_size[axis] += leftover * child->pref_size[axis].value / weights;
        }
      }
    }
  } else {
    //- across the child axis a grow child fills the parent's content
    for(UI_Box* child = box->first; child != 0; child = child->next) {
      if(child->flags & UI_BoxFlag_Floating) { continue; }
      if(child->pref_size[axis].kind == UI_SizeKind_Grow) {
        child->fixed_size[axis] = content;
      }
    }
  }
  for(UI_Box* child = box->first; child != 0; child = child->next) {
    ui__layout_grow(child, axis);
  }
}

internal void ui__layout_violations(UI_Box* box, UI_Axis axis) {
  F32 content = box->fixed_size[axis] - 2 * ui__v2_axis(box->padding, axis);
  if(axis == box->child_axis) {
    //- children overflow the parent along the layout axis: take back the
    //  deficit proportionally to size * (1 - strictness)
    F32 total = 0;
    U64 n = 0;
    for(UI_Box* child = box->first; child != 0; child = child->next) {
      if(child->flags & UI_BoxFlag_Floating) { continue; }
      total += child->fixed_size[axis];
      n += 1;
    }
    if(n > 1) {
      total += box->child_gap * (F32)(n - 1);
    }
    F32 over = total - content;
    if(over > 0) {
      F32 budget = 0;
      for(UI_Box* child = box->first; child != 0; child = child->next) {
        if(child->flags & UI_BoxFlag_Floating) { continue; }
        budget += child->fixed_size[axis] * (1 - child->pref_size[axis].strictness);
      }
      if(budget > 0) {
        F32 f = Min(1.0f, over / budget);
        for(UI_Box* child = box->first; child != 0; child = child->next) {
          if(child->flags & UI_BoxFlag_Floating) { continue; }
          F32 give = child->fixed_size[axis] * (1 - child->pref_size[axis].strictness) * f;
          child->fixed_size[axis] = ClampBot(child->fixed_size[axis] - give, 0);
        }
      }
    }
  } else {
    //- off-axis: each child clamps toward the parent's content size
    for(UI_Box* child = box->first; child != 0; child = child->next) {
      if(child->flags & UI_BoxFlag_Floating) { continue; }
      F32 over = child->fixed_size[axis] - content;
      if(over > 0) {
        F32 give = over * (1 - child->pref_size[axis].strictness);
        child->fixed_size[axis] = ClampBot(child->fixed_size[axis] - give, 0);
      }
    }
  }
  for(UI_Box* child = box->first; child != 0; child = child->next) {
    ui__layout_violations(child, axis);
  }
}

// breaks every DrawText box's string into drawn lines: on '\n' always, and
// greedily on word boundaries against the solved width when the box wraps.
// A word wider than the available width gets a line alone and overflows.
internal void ui__build_lines(UI_Box* box) {
  if(box->flags & UI_BoxFlag_DrawText) {
    String8 s = box->string;
    U64 cap = 1;
    for(U64 i = 0; i < s.size; i += 1) {
      cap += (s.str[i] == ' ' || s.str[i] == '\n');
    }
    box->lines = push_array_no_zero(ui_state.arena, String8, cap);
    box->line_count = 0;
    F32 avail = box->fixed_size[UI_Axis_X] - 2 * box->padding.x;
    U64 start = 0;
    for(U64 i = 0; i <= s.size; i += 1) {
      if(i == s.size || s.str[i] == '\n') {
        String8 para = str8_substr(s, start, i);
        if(!(box->flags & UI_BoxFlag_TextWrap) || para.size == 0) {
          box->lines[box->line_count] = para;
          box->line_count += 1;
        } else {
          U64 pos = 0;
          while(pos < para.size) {
            while(pos < para.size && para.str[pos] == ' ') { pos += 1; }
            if(pos >= para.size) { break; }
            U64 line_start = pos;
            U64 line_end = pos;
            for(U64 scan = pos; scan < para.size;) {
              while(scan < para.size && para.str[scan] == ' ') { scan += 1; }
              U64 word_start = scan;
              while(scan < para.size && para.str[scan] != ' ') { scan += 1; }
              if(word_start == scan) { break; }
              String8 cand = str8_substr(para, line_start, scan);
              if(line_end == line_start ||
                 ui__measure_text(box->font, box->font_size, cand).x <= avail) {
                line_end = scan;
              } else {
                break;
              }
            }
            box->lines[box->line_count] = str8_substr(para, line_start, line_end);
            box->line_count += 1;
            pos = line_end;
          }
        }
        start = i + 1;
      }
    }
    Assert(box->line_count <= cap);
    ui_state.line_total += box->line_count;
  }
  for(UI_Box* child = box->first; child != 0; child = child->next) {
    ui__build_lines(child);
  }
}

internal void ui__layout_position(UI_Box* box) {
  UI_Axis on = box->child_axis;
  UI_Axis off = (on == UI_Axis_X) ? UI_Axis_Y : UI_Axis_X;
  F32 align = (box->child_align == UI_Align_Center) ? 0.5f
              : (box->child_align == UI_Align_End)  ? 1.0f
                                                    : 0.0f;
  F32 off_content = box->fixed_size[off] - 2 * ui__v2_axis(box->padding, off);
  V2 cursor = v2_add(box->rect.min, box->padding);
  for(UI_Box* child = box->first; child != 0; child = child->next) {
    V2 p = cursor;
    if(child->flags & UI_BoxFlag_Floating) {
      V2 a = child->floating_anchor;
      p = (V2){box->rect.min.x + a.x * (box->fixed_size[UI_Axis_X] - child->fixed_size[UI_Axis_X]) + child->floating_pos.x,
               box->rect.min.y + a.y * (box->fixed_size[UI_Axis_Y] - child->fixed_size[UI_Axis_Y]) + child->floating_pos.y};
    } else {
      // cross-axis alignment: children keep the padding edge only when
      // aligned Start; oversized children stay pinned to it
      F32 slack = ClampBot(off_content - child->fixed_size[off], 0) * align;
      if(off == UI_Axis_X) {
        p.x += slack;
      } else {
        p.y += slack;
      }
      if(on == UI_Axis_X) {
        cursor.x += child->fixed_size[UI_Axis_X] + box->child_gap;
      } else {
        cursor.y += child->fixed_size[UI_Axis_Y] + box->child_gap;
      }
    }
    child->rect = (Rect){p, {p.x + child->fixed_size[UI_Axis_X], p.y + child->fixed_size[UI_Axis_Y]}};
    ui__layout_position(child);
  }
}

////////////////////////////////
//~ fp: Command Emission
//
// One preorder walk in paint order: background, clip push, text, children,
// clip pop, border (so borders sit over child spill). The same walk updates
// each box's persistent rect and animation, and resolves next frame's hot
// box -- the last hit in paint order is the topmost.

internal UI_DrawCommand* ui__push_cmd(UI_DrawCommandKind kind) {
  UI_State* s = &ui_state;
  Assert(s->cmd_count < s->cmd_cap);
  UI_DrawCommand* cmd = &s->cmds[s->cmd_count];
  s->cmd_count += 1;
  MemoryZeroStruct(cmd);
  cmd->kind = kind;
  return cmd;
}

internal void ui__emit(UI_Box* box) {
  UI_State* s = &ui_state;

  if(box->state != 0) {
    UI_BoxState* state = box->state;
    state->rect = box->rect;
    state->rect_valid = 1;
    F32 blend = 1.0f - f32_exp2(-24.0f * s->input.dt);
    F32 hot_target = (s->hot_key == box->key) ? 1.0f : 0.0f;
    F32 active_target = (s->active_key == box->key) ? 1.0f : 0.0f;
    state->hot_t += (hot_target - state->hot_t) * blend;
    state->active_t += (active_target - state->active_t) * blend;
  }

  if(ui__rect_contains(box->rect, s->input.mouse)) {
    if((box->flags & UI_BoxFlag_Clickable) && box->key != 0) {
      s->hot_key_next = box->key;
    }
    if(box->flags & (UI_BoxFlag_Clickable | UI_BoxFlag_DrawBackground)) {
      s->mouse_over = 1;
    }
  }

  if(box->flags & UI_BoxFlag_DrawBackground) {
    V4 bg = box->background_color;
    if((box->flags & UI_BoxFlag_Clickable) && box->state != 0) {
      // the theme's hover/active colors pull the background toward
      // themselves as the box heats up / is held; alpha is the strength.
      // Resolved here, after the tag stack unwound -- the box's tags_key
      // still names the context it was built under
      V4 hover = ui__color_from_key_name(box->tags_key, str8_lit("hover"));
      V4 active = ui__color_from_key_name(box->tags_key, str8_lit("active"));
      F32 fh = hover.w * box->state->hot_t;
      F32 fa = active.w * box->state->active_t;
      bg.x += (hover.x - bg.x) * fh + (active.x - bg.x) * fa;
      bg.y += (hover.y - bg.y) * fh + (active.y - bg.y) * fa;
      bg.z += (hover.z - bg.z) * fh + (active.z - bg.z) * fa;
    }
    UI_DrawCommand* cmd = ui__push_cmd(UI_DrawCommandKind_Rect);
    cmd->rect = box->rect;
    for(Corner c = 0; c < Corner_COUNT; c += 1) {
      cmd->colors[c] = bg;
      cmd->corner_radii[c] = box->corner_radius;
    }
    cmd->edge_softness = box->corner_radius > 0 ? 1.0f : 0.0f;
  }

  if(box->flags & UI_BoxFlag_Clip) {
    UI_DrawCommand* cmd = ui__push_cmd(UI_DrawCommandKind_ClipPush);
    cmd->rect = box->rect;
  }

  if((box->flags & UI_BoxFlag_DrawText) && box->line_count > 0) {
    UI_FontMetrics m = ui__font_metrics(box->font, box->font_size);
    V2 tl = v2_add(box->rect.min, box->padding);
    F32 content_w = box->fixed_size[UI_Axis_X] - 2 * box->padding.x;
    for(U64 i = 0; i < box->line_count; i += 1) {
      String8 line = box->lines[i];
      if(line.size == 0) { continue; }
      F32 x = tl.x;
      if(box->text_align != UI_TextAlign_Left) {
        F32 w = ui__measure_text(box->font, box->font_size, line).x;
        x += (box->text_align == UI_TextAlign_Center) ? (content_w - w) * 0.5f
                                                      : (content_w - w);
      }
      UI_DrawCommand* cmd = ui__push_cmd(UI_DrawCommandKind_Text);
      cmd->font = box->font;
      cmd->font_size = box->font_size;
      cmd->pos = (V2){x, tl.y + (F32)i * m.line_advance};
      cmd->color = box->text_color;
      cmd->text = line;
    }
  }

  for(UI_Box* child = box->first; child != 0; child = child->next) {
    ui__emit(child);
  }

  if(box->flags & UI_BoxFlag_Clip) {
    ui__push_cmd(UI_DrawCommandKind_ClipPop);
  }

  if((box->flags & UI_BoxFlag_DrawBorder) && box->border_thickness > 0) {
    UI_DrawCommand* cmd = ui__push_cmd(UI_DrawCommandKind_Rect);
    cmd->rect = box->rect;
    for(Corner c = 0; c < Corner_COUNT; c += 1) {
      cmd->colors[c] = box->border_color;
      cmd->corner_radii[c] = box->corner_radius;
    }
    cmd->border_thickness = box->border_thickness;
    cmd->edge_softness = 1.0f;
  }
}

internal UI_DrawList ui_frame_end(void) {
  UI_State* s = &ui_state;
  Assert(s->parent_stack.top == 1); // begin/end pairs must balance
  UI_Box* root = s->root;

  ui__layout_standalone(root, UI_Axis_X);
  ui__layout_pct(root, UI_Axis_X);
  ui__layout_sum(root, UI_Axis_X);
  ui__layout_grow(root, UI_Axis_X);
  ui__layout_violations(root, UI_Axis_X);
  ui__build_lines(root);
  ui__layout_standalone(root, UI_Axis_Y);
  ui__layout_pct(root, UI_Axis_Y);
  ui__layout_sum(root, UI_Axis_Y);
  ui__layout_grow(root, UI_Axis_Y);
  ui__layout_violations(root, UI_Axis_Y);
  root->rect = (Rect){{0, 0}, {root->fixed_size[UI_Axis_X], root->fixed_size[UI_Axis_Y]}};
  ui__layout_position(root);

  s->cmd_cap = s->box_count * 4 + s->line_total + 8;
  s->cmds = push_array_no_zero(s->arena, UI_DrawCommand, s->cmd_cap);
  s->cmd_count = 0;
  ui__emit(root);

  if(s->input.released[UI_MouseButton_Left]) {
    s->active_key = 0;
  }

  UI_DrawList list = {0};
  list.commands = s->cmds;
  list.count = s->cmd_count;
  list.mouse_over_ui = s->mouse_over || s->active_key != 0;
  return list;
}

////////////////////////////////
//~ fp: Widgets

internal void ui_label(String8 string) {
  UI_Box* box = ui_box(UI_BoxFlag_DrawText, string);
  box->pref_size[UI_Axis_X] = ui_size_text(1);
  box->pref_size[UI_Axis_Y] = ui_size_text(1);
}

internal void ui_labelf(char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  String8 string = push_str8fv(ui_state.arena, fmt, args);
  va_end(args);
  ui_label(string);
}

internal void ui_text_wrapped(String8 string) {
  UI_Box* box = ui_box(UI_BoxFlag_DrawText | UI_BoxFlag_TextWrap, string);
  box->pref_size[UI_Axis_X] = ui_size_pct(1, 0);
  box->pref_size[UI_Axis_Y] = ui_size_text(1);
}

internal UI_Signal ui_button(String8 string) {
  ui_push_tag(str8_lit("button"));
  UI_Box* box = ui_box(UI_BoxFlag_Clickable | UI_BoxFlag_DrawBackground |
                           UI_BoxFlag_DrawBorder | UI_BoxFlag_DrawText,
                       string);
  ui_pop_tag();
  box->pref_size[UI_Axis_X] = ui_size_text(0);
  box->pref_size[UI_Axis_Y] = ui_size_text(1);
  box->padding = (V2){10, 5};
  box->corner_radius = 8;
  box->border_thickness = 2;
  box->text_align = UI_TextAlign_Center;
  return ui_signal(box);
}

internal void ui_spacer(UI_Size size) {
  UI_Box* parent = ui__top(parent_stack);
  UI_Box* box = ui_box(0, str8_lit(""));
  UI_Axis off = (parent->child_axis == UI_Axis_X) ? UI_Axis_Y : UI_Axis_X;
  box->pref_size[parent->child_axis] = size;
  box->pref_size[off] = ui_size_points(0, 0);
}

internal void ui_tooltip(String8 string) {
  // parented to the root: floating_pos is parent-relative, and the mouse is
  // window-space -- only the root's origin makes those coincide. Also keeps
  // the tip clear of any panel clip.
  ui_push_parent(ui_root());
  ui_push_tag(str8_lit("tooltip"));
  UI_Box* tip = ui_box(UI_BoxFlag_DrawBackground | UI_BoxFlag_DrawBorder |
                           UI_BoxFlag_DrawText | UI_BoxFlag_Floating,
                       string);
  ui_pop_tag();
  ui_pop_parent();
  tip->floating_pos = v2_add(ui_mouse(), (V2){16, 18});
  tip->padding = (V2){8, 6};
  tip->corner_radius = 8;
  tip->pref_size[UI_Axis_X] = ui_size_text(1);
  tip->pref_size[UI_Axis_Y] = ui_size_text(1);
}

internal UI_Box* ui_row_begin(String8 string) {
  UI_Box* box = ui_box(0, string);
  box->child_axis = UI_Axis_X;
  ui_push_parent(box);
  return box;
}

internal void ui_row_end(void) {
  ui_pop_parent();
}

internal UI_Box* ui_column_begin(String8 string) {
  UI_Box* box = ui_box(0, string);
  box->child_axis = UI_Axis_Y;
  ui_push_parent(box);
  return box;
}

internal void ui_column_end(void) {
  ui_pop_parent();
}

internal UI_Box* ui_panel_begin(String8 string) {
  ui_push_tag(str8_lit("panel")); // spans the children until ui_panel_end
  UI_Box* box = ui_box(UI_BoxFlag_DrawBackground | UI_BoxFlag_DrawBorder | UI_BoxFlag_Clip, string);
  box->child_axis = UI_Axis_Y;
  box->corner_radius = 10;
  ui_push_parent(box);
  return box;
}

internal void ui_panel_end(void) {
  ui_pop_parent();
  ui_pop_tag();
}

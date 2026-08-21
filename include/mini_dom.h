/*
 * mini_dom.h — self-written minimal DOM tree + reduced layout engine.
 *
 * NOT a browser DOM. A purpose-built scene-graph that exposes just enough
 * of the Node/Element API surface for Vue 3 / React 18 vDOM diffs to drive,
 * plus a deliberately *reduced* layout (block flow + flexbox only).
 *
 * Why a custom tree instead of libxml2 etc.: we need O(1) parent/child/
 * sibling pointers, a style map keyed by the ~20 CSS props that actually
 * affect layout, and a layout pass that writes geometry back into the same
 * node. A general XML/HTML parser would carry layout-irrelevant weight.
 */
#ifndef MINI_DOM_H
#define MINI_DOM_H

#include <stdint.h>
#include "mini_renderer.h"

#ifdef __cplusplus
extern "C"
{
#endif

   /* ------------------------------------------------------------------ */
   /* Node types (mirrors DOM NodeType numbers so JS === checks pass)    */
   /* The full DOM hierarchy: EventTarget -> Node -> {Document, Element   */
   /* (-> HTMLElement -> SVGElement), CharacterData (-> Text/Comment),    */
   /* DocumentFragment, Attr}. Each maps to one MiniNodeType below; the   */
   /* tag string carries the element subclass (HTMLDivElement etc. is just */
   /* an Element whose tag == "div").                                    */
   /* ------------------------------------------------------------------ */
   typedef enum
   {
      MN_ELEMENT_NODE = 1,
      MN_ATTRIBUTE_NODE = 2, /* Attr                                   */
      MN_TEXT_NODE = 3,      /* CharacterData -> Text                  */
      MN_CDATA_SECTION_NODE = 4,
      MN_PROCESSING_INSTRUCTION = 7,
      MN_COMMENT_NODE = 8, /* CharacterData -> Comment               */
      MN_DOCUMENT_NODE = 9,
      MN_DOCUMENT_TYPE_NODE = 10,     /* <!DOCTYPE ...>                          */
      MN_DOCUMENT_FRAGMENT_NODE = 11, /* DocumentFragment / ShadowRoot          */
      MN_NOTATION_NODE = 12
   } MiniNodeType;

   /* ------------------------------------------------------------------ */
   /* Native HTML element classification — the 8 functional families the  */
   /* HTML spec groups elements into. Drives the parser's void/raw-text   */
   /* tables, the layout's default display, and the renderer's per-tag    */
   /* placeholder drawing. This is the single registry that grows when a   */
   /* new tag is recognized; everything else is data-driven from it.       */
   /* ------------------------------------------------------------------ */
   typedef enum
   {
      MINI_CAT_UNKNOWN = 0,
      MINI_CAT_METADATA = 1,   /* html/head/title/meta/link/style/script/base */
      MINI_CAT_SECTION = 2,    /* body/header/footer/nav/main/article/section/aside/h1-h6/hgroup */
      MINI_CAT_TEXT = 3,       /* div/span/p/a/strong/b/em/i/code/pre/blockquote/q/br/hr/mark/small/sub/sup/time/... */
      MINI_CAT_LIST = 4,       /* ul/ol/li/dl/dt/dd/menu                       */
      MINI_CAT_FORM = 5,       /* form/input/button/textarea/select/option/optgroup/label/fieldset/legend/datalist/progress/meter/output */
      MINI_CAT_MEDIA = 6,      /* img/audio/video/source/track/iframe/canvas/svg/embed/object/param/picture/map/area/math */
      MINI_CAT_TABLE = 7,      /* table/caption/thead/tbody/tfoot/tr/th/td/col/colgroup */
      MINI_CAT_INTERACTIVE = 8 /* details/summary/dialog/template/slot/portal  */
   } MiniElementCategory;

   /* Display model a tag defaults to (CSS `display` is overlaid on top). */
   typedef enum
   {
      MINI_DISPLAY_BLOCK = 0,
      MINI_DISPLAY_FLEX = 1,
      MINI_DISPLAY_NONE = 2,
      MINI_DISPLAY_INLINE = 3,
      MINI_DISPLAY_TABLE = 4,
      MINI_DISPLAY_LIST_ITEM = 5,
      MINI_DISPLAY_INLINE_FLEX = 6
   } MiniDisplayModel;

   typedef struct MiniElementInfo
   {
      MiniElementCategory category;
      uint8_t is_void;         /* no end tag, no children (br, img, input...)   */
      uint8_t is_raw_text;     /* content parsed verbatim (script/style/textarea/title) */
      uint8_t heading_level;   /* 1..6 for h1-h6, else 0                        */
      uint8_t default_display; /* MiniDisplayModel the tag renders as by default */
      uint8_t form_type;       /* parsed <input type=...> family, 0=none        */
   } MiniElementInfo;

   /* Resolve everything the parser/layout/render passes need to know about
      a tag name in one lookup. Unknown tags return MINI_CAT_UNKNOWN with
      default_display=BLOCK so custom elements still render as a box. */
   const MiniElementInfo *mini_element_info(const char *tag);

   /* ------------------------------------------------------------------ */
   /* Style map — only properties the layout/render passes consume.      */
   /* Extending here is the single place CSS support grows.              */
   /* ------------------------------------------------------------------ */
   /* A CSS length before resolution. unit: 0=px (or unitless), 1=%,
      2=em, 3=rem, 4=vw, 5=vh, 6=vmin, 7=vmax. Resolved to px in the
      layout pass where the percentage / em / rem / viewport context is
      known. */
   typedef struct
   {
      float v;
      uint8_t unit;
   } MiniLength;

   typedef struct MiniStyle
   {
      /* box model (px) */
      float x, y, w, h; /* computed layout result (filled by layout) */
      float margin[4];  /* top right bottom left                   */
      float padding[4];
      float border_w[4];                                   /* per-side width: top right bottom left      */
      uint8_t border_style[4];                             /* 0=none 1=solid 2=dashed 3=dotted        */
      float border_r, border_g, border_b, border_a;        /* single border color */
      uint8_t has_border;                                  /* any border property was set → draw check    */
      uint8_t overflow;                                    /* 0=visible, 1=clip (hidden/scroll/auto)     */
      int position;                                        /* 0=static 1=relative 2=absolute 3=fixed 4=sticky */
      MiniLength len_top, len_left, len_right, len_bottom; /* insets       */
      int z_index;                                         /* paint order within a stacking context        */
      uint8_t box_sizing;                                  /* 0=content-box (default), 1=border-box       */
      /* flex */
      int display;         /* 0=block, 1=flex, 2=none                 */
      int flex_direction;  /* 0=row, 1=col                          */
      int justify_content; /* enum: flex-start..space-around (0..5)   */
      int align_items;     /* 0=stretch..4=baseline                   */
      float flex_grow;
      /* paint */
      float bg_r, bg_g, bg_b, bg_a;
      float color_r, color_g, color_b, color_a;
      float font_size;
      /* computed absolute position filled by layout pass */
      float abs_x, abs_y;
      int laid_out;

      /* raw CSS lengths (resolved into w/h/margin/padding/font_size by the
         layout pass; px values resolve to themselves, %/em/rem/vh/vw need
         the layout context that only exists at layout time).              */
      MiniLength len_w, len_h;
      MiniLength len_margin[4], len_padding[4];
      MiniLength len_font;
      MiniLength len_min_w, len_max_w, len_min_h, len_max_h;
      MiniLength len_gap; /* flex gap (row/column-gap share this) */
      int align_self;     /* -1=auto (use align_items), else 0..4 like align_items */
      uint8_t font_set;   /* font-size explicitly set (else keep the default) */

      /* --- New Modern CSS Properties --- */
      float border_radius;
      uint8_t border_radius_pct;      /* 0=px, 1=% (resolve to min(w,h)/2 at render) */
      float border_radius_corners[4]; /* TL, TR, BR, BL */
      uint8_t border_radius_pct_corners[4];

      /* Shadow (box-shadow) */
      uint8_t has_shadow;
      float shadow_x, shadow_y, shadow_blur, shadow_spread;
      float shadow_r, shadow_g, shadow_b, shadow_a;
#define MINI_MAX_SHADOWS 4
      struct
      {
         float x, y, blur, spread;
         float r, g, b, a;
         uint8_t inset;
      } shadows[MINI_MAX_SHADOWS];
      int num_shadows;

      /* Gradient (linear-gradient) */
      uint8_t has_gradient;
      float grad_r1, grad_g1, grad_b1, grad_a1;
      float grad_r2, grad_g2, grad_b2, grad_a2;
      int grad_vertical;
      int grad_type;    /* 0=linear 1=radial 2=conic */
      float grad_angle; /* degrees (linear) */
#define MINI_MAX_GRAD_STOPS 16
      struct
      {
         float r, g, b, a;
         float pos; /* 0..1 or -1 if auto */
      } grad_stops[MINI_MAX_GRAD_STOPS];
      int grad_num_stops;

      /* text gradient: background-clip:text / -webkit-text-fill-color:transparent */
      uint8_t text_gradient;

      /* CSS Grid */
      uint8_t is_grid;
      int grid_cols; /* explicit grid-template-columns count; 0=auto */
      int grid_col_start, grid_col_span;
      int grid_row_start, grid_row_span;
      char grid_area[64];
      char grid_gtc[128];
      char grid_gtr[128];
      int grid_auto_flow_dense;
      char grid_areas[256];

      /* Flexbox extensions */
      int flex_wrap;     /* 0=nowrap, 1=wrap, 2=wrap-reverse */
      float flex_shrink; /* default 1.0 */
      MiniLength len_flex_basis;
      uint8_t flex_basis_set;
      int order;         /* Flex item order: default 0 */
      int align_content; /* 0=stretch 1=flex-start 2=flex-end 3=center 4=space-between 5=space-around 6=space-evenly */

      /* Typography & Text */
      int font_weight;     /* 400=normal, 700=bold, 800/900 */
      int text_align;      /* 0=left, 1=center, 2=right, 3=justify */
      int text_decoration; /* 0=none, 1=underline, 2=line-through, 3=overline */
      int white_space;     /* 0=normal, 1=nowrap, 2=pre, 3=pre-wrap */
      int word_break;      /* 0=normal, 1=break-all, 2=keep-all */
      int text_overflow;   /* 0=clip, 1=ellipsis */
      int vertical_align;  /* 0=baseline 1=middle 2=top 3=bottom 4=sub 5=super */

      /* Background image */
      char bg_image_url[256];
      int bg_size_mode; /* 0=auto, 1=cover, 2=contain, 3=explicit */
      MiniLength bg_size_w, bg_size_h;
      MiniLength bg_pos_x, bg_pos_y;
      int bg_repeat; /* 0=repeat, 1=no-repeat, 2=repeat-x, 3=repeat-y */

      /* --- Phase 4: modern CSS (opacity, cursor, text, filters, 3D, transitions) --- */
      float opacity; /* 0..1; default 1 (has_opacity gates) */
      uint8_t has_opacity;
      int pointer_events;    /* 0=auto, 1=none (hit-test passthrough) */
      int cursor;            /* 0=default 1=pointer 2=text 3=grab 4=... */
      uint8_t user_select;   /* 0=auto, 1=none, 2=text, 3=all */
      MiniLength len_letter; /* letter-spacing */
      uint8_t letter_set;
      MiniLength len_line_height; /* line-height */
      uint8_t line_height_set;
      int text_transform; /* 0=none 1=uppercase 2=lowercase 3=capitalize */
      uint8_t font_style; /* 0=normal 1=italic 2=oblique */
      int mix_blend_mode; /* 0=normal 1=multiply 2=screen 3=lighter ... */

      /* 2D / 3D transform */
      uint8_t has_transform;
      float translate_x, translate_y, translate_z;
      float rotate_x, rotate_y, rotate_z; /* radians */
      float scale_x, scale_y, scale_z;
      float skew_x, skew_y;
      float perspective;                            /* 0 = none */
      uint8_t transform_style_3d;                   /* 0=flat, 1=preserve-3d */
      float transform_origin_x, transform_origin_y; /* 0..1 fractions */

      /* filters (parse + store; GPU post-process = renderer) */
      uint8_t has_backdrop_filter;
      float backdrop_blur; /* px */
      uint8_t has_filter;
      float filter_blur; /* px */
      float filter_invert;
      float filter_grayscale;
      float filter_brightness;

      /* sticky positioning */
      float sticky_top, sticky_bottom, sticky_left, sticky_right;
      uint8_t has_sticky_top, has_sticky_bottom, has_sticky_left, has_sticky_right;

      /* transition */
      uint8_t has_transition;
      float transition_duration; /* seconds */
      int transition_timing;     /* 0=linear 1=ease 2=ease-in 3=ease-out 4=ease-in-out 5=cubic-bezier */
      float transition_bezier[4];
      char transition_prop[32];
#define MINI_MAX_TRANSITIONS 8
      struct
      {
         char prop[32];
         float duration;
         float delay;
         int timing;
         float bezier[4];
      } transitions[MINI_MAX_TRANSITIONS];
      int num_transitions;

      /* CSS Animation */
      uint8_t has_animation;
      char anim_name[64];
      float anim_duration;
      float anim_delay;
      int anim_timing; /* 0=linear 1=ease 2=ease-in 3=ease-out 4=ease-in-out 5=cubic-bezier */
      float anim_bezier[4];
      int anim_iteration_count; /* -1 = infinite, >0 = count */
      int anim_direction;       /* 0=normal 1=reverse 2=alternate 3=alternate-reverse */
      int anim_fill_mode;       /* 0=none 1=forwards 2=backwards 3=both */
      int anim_play_state;      /* 0=running 1=paused */
      /* Animation tick memoization — let apply_animations skip work on frames
         where nothing visibly changes:
         - anim_last_eased: the eased progress applied last tick (<0 = never).
           Steps-timing animations (e.g. a cursor blink) hold the same eased
           value for many frames; skipping the keyframe-body re-apply + the
           mini_style_set churn on those idle frames is a big steady-state win.
         - anim_completed: a finite animation (iteration_count>0) that has
           reached its iteration limit. Previously the engine ignored
           iteration_count and looped ALL animations forever (fmod wraps),
           so a one-shot `forwards` reveal re-animated every cycle and drove
           active_effects at 60 Hz forever. Now it completes once, holds its
           end state, and stops forcing frames. */
      float anim_last_eased;
      uint8_t anim_completed;

      /* text-shadow (single layer) */
      uint8_t has_text_shadow;
      float ts_x, ts_y, ts_blur;
      float ts_r, ts_g, ts_b, ts_a;

      /* clip-path & clip: rect() */
      uint8_t has_clip_polygon;
      int num_clip_poly_pts;
      float clip_poly_pts[8][2];
      uint8_t has_clip_rect;
      float clip_rect[4]; /* top, right, bottom, left */
   } MiniStyle;

   /* ------------------------------------------------------------------ */
   /* Attribute (name/value string pair, singly-linked)                  */
   /* ------------------------------------------------------------------ */
   typedef struct MiniAttr
   {
      char *name;
      char *value;
      struct MiniAttr *next;
   } MiniAttr;

   /* Scoped CSS custom variable */
   typedef struct MiniNodeVar
   {
      char *name;
      char *value;
      struct MiniNodeVar *next;
   } MiniNodeVar;

   /* Active Transition State */
   typedef struct MiniActiveTransition
   {
      char prop[32];
      float start_val[8];
      float target_val[8];
      int num_vals;
      double start_time;
      double duration;
      int timing;
      float bezier[4];
      struct MiniActiveTransition *next;
   } MiniActiveTransition;

   /* ------------------------------------------------------------------ */
   /* The node                                                            */
   /* ------------------------------------------------------------------ */
   typedef struct MiniNode MiniNode;
   struct MiniNode
   {
      MiniNodeType type;
      char *tag;       /* element tag, or NULL for text/doc      */
      char *text;      /* text content (text nodes / textContent) */
      MiniAttr *attrs; /* attribute list                         */
      MiniStyle style; /* resolved style + computed geometry     */

      struct MiniNode *parent;
      struct MiniNode *first_child;
      struct MiniNode *last_child;
      struct MiniNode *prev_sibling;
      struct MiniNode *next_sibling;

      /* Pseudo element nodes (::before, ::after) */
      struct MiniNode *pseudo_before;
      struct MiniNode *pseudo_after;

      /* Scoped CSS custom properties */
      MiniNodeVar *vars;

      /* Active transitions */
      MiniActiveTransition *active_transitions;

      /* JS-side listeners are owned by the bridge; here we just store the
         count so hasListener checks are O(1) in the hot layout path.       */
      int listener_count;

      /* Stable CDP node id (assigned by mini_dom_assign_node_ids, 0 = none).
         The Elements panel addresses nodes by this id across requests. */
      int cdp_node_id;

      /* Web Components: a shadow root is a DocumentFragment-mode node that
         replaces this element's light-tree children at render time. NULL
         for plain elements. <slot> inside the shadow tree pulls in the
         element's light children.                                         */
      struct MiniNode *shadow_root;
      /* slot assignment: for <slot> elements, points at the slotted light
         node whose children should be rendered in place of the slot.     */
      struct MiniNode *assigned_slot;
      void *js_wrapper;

      /* cached classification (set by mini_node_create_element) so the
         render/layout dispatch avoids a per-frame strcmp storm.          */
      uint8_t category;
      uint8_t default_display;
      uint8_t heading_level;

      MiniStyle base_style;
      uint8_t has_base_style;

      /* ---- Phase 4: interaction state + dirty marks + pseudo-content ---- */
      /* interaction state: set by mini_events.c (hit-test / focus) and read
         by match_pseudo() so :hover/:active/:focus rules apply at restyle. */
      uint8_t state_hovered;
      uint8_t state_active;
      uint8_t state_focused;
      /* Text caret byte offset into the value (for <input>/<textarea>).
         -1 = no caret set; the events layer sets/clamps it on the focused
         control and the renderer draws a blinking caret at this offset.
         Kept here so render_input/render_textarea (which only see the node)
         can paint the caret without a back-pointer to the event state.       */
      int caret_offset;
      /* In-field selection anchor (byte offset) paired with caret_offset:
         when >= 0 the renderer paints a selection rect between sel_anchor_off
         and caret_offset. -1 = no in-field selection (caret only). Shift+
         arrow / Shift+click sets it; plain caret movement clears it.         */
      int sel_anchor_off;
      /* dirty marks: set when style/DOM mutates so mini_dom_tick_frame can
         drive an incremental relayout/repaint instead of doing it eagerly. */
      uint8_t dirty_layout;
      uint8_t dirty_paint;
      /* ::before / ::after generated content (owned by the node; freed in
         mini_node_destroy). NULL = no generated content for that edge.    */
      char *before_content;
      char *after_content;
      /* opaque calc() side-store (MiniCalc* defined in mini_dom.c). Holds
         deferred calc() expressions for length fields whose value can only
         be resolved at layout time (e.g. calc(100% - 20px)).              */
      void *calc;
   };

   /* ------------------------------------------------------------------ */
   /* Linear Arena Allocator (Frame Arena & Document Arena)               */
   /* ------------------------------------------------------------------ */
   typedef struct MiniArenaBlock
   {
      struct MiniArenaBlock *next;
      size_t capacity;
      size_t used;
      uint8_t data[];
   } MiniArenaBlock;

   typedef struct MiniArena
   {
      MiniArenaBlock *head;
      MiniArenaBlock *current;
      size_t default_block_size;
   } MiniArena;

   void mini_arena_init(MiniArena *arena, size_t default_block_size);
   void *mini_arena_alloc(MiniArena *arena, size_t size);
   char *mini_arena_strdup(MiniArena *arena, const char *s);
   char *mini_arena_strndup(MiniArena *arena, const char *s, size_t n);
   void mini_arena_reset(MiniArena *arena);
   void mini_arena_destroy(MiniArena *arena);

   typedef struct MiniDocument
   {
      struct MiniNode *root; /* DOCUMENT_NODE                          */
      struct MiniNode *body; /* convenience <body> pointer             */
      float scroll_x;
      float scroll_y;
      float max_scroll_x;
      float max_scroll_y;
      int viewport_w;
      int viewport_h;
      MiniArena doc_arena;
      MiniArena frame_arena;
      void *ctx;             /* Internal Document Context               */
      /* Per-frame activity summary, produced by mini_dom_tick_frame's
         existing tree walk at zero extra traversal cost: `dirty` ORs every
         node's dirty_layout/dirty_paint (hover/mutation/animation changes
         since the last rendered frame); `active_effects` is set while any
         CSS transition or @keyframes animation is still running.

         The host loop reads these to skip the expensive
         restyle + layout + render + flush + swap pipeline when nothing
         needs a new frame — previously the loop did a full-pipeline spin
         every frame even on a fully static page, which was the dominant
         CPU drain. Per-node dirty is cleared at the end of each tick so
         the summary stays fresh; nothing currently reads per-node dirty
         to branch on, so clearing it is safe. */
      uint8_t dirty;
      uint8_t active_effects;
      /* Layout re-run gate. Set whenever a style/DOM/viewport mutation
         changes geometry (mini_style_set for layout-affecting props,
         mini_dom_restyle, viewport resize). Cleared at the top of
         mini_layout_run. The host loop gates mini_layout_run on
         (doc->dirty || doc->layout_dirty) so that pure paint-only
         animation frames (e.g. an opacity blink) skip the full layout
         pass entirely — previously layout ran every frame unconditionally,
         the dominant steady-state CPU drain on animation-heavy pages
         (grid template re-parse, double-lay of children, etc.). */
      uint8_t layout_dirty;
      /* Paint re-run gate. Set by apply_animations / tick_transitions only
         when a value actually changed this tick (NOT merely because an
         animation exists). Lets the host skip the full clear+render+flush
         on animation frames where the eased progress is unchanged (e.g. a
         steps(2) cursor blink holds the same value for ~33/34 frames). */
      uint8_t paint_dirty;
   } MiniDocument;

   /* ------------------------------------------------------------------ */
   /* Tree lifecycle                                                      */
   /* ------------------------------------------------------------------ */
   MiniDocument *mini_doc_create(void);
   void mini_doc_destroy(MiniDocument *doc);

   struct MiniNode *mini_node_create_element(const char *tag);
   struct MiniNode *mini_node_create_text(const char *data);
   void mini_node_destroy(struct MiniNode *n);

   /* Tree mutation (return 0 on success, mirrors DOM return semantics). */
   int mini_node_append_child(struct MiniNode *parent, struct MiniNode *child);
   int mini_node_insert_before(struct MiniNode *parent, struct MiniNode *new_child,
                               struct MiniNode *ref);
   int mini_node_remove_child(struct MiniNode *parent, struct MiniNode *child);

   /* Attributes / text */
   void mini_node_set_attribute(struct MiniNode *n, const char *k, const char *v);
   void mini_node_remove_attribute(struct MiniNode *n, const char *k);
   const char *mini_node_get_attribute(const struct MiniNode *n, const char *k);
   void mini_node_set_text(struct MiniNode *n, const char *t);

   /* Scoped CSS custom properties */
   void mini_node_set_var(struct MiniNode *n, const char *name, const char *value);
   const char *mini_node_get_var(const struct MiniNode *n, const char *name);

   /* Style resolver: takes a CSS-style key/value (e.g. "display","flex")
      and writes into the node style. Unknown props are silently dropped.   */
   void mini_style_set(struct MiniNode *n, const char *prop, const char *val);
   void mini_style_set_base(struct MiniNode *n, const char *prop, const char *val);

   /* Color parsing (shared by CSS + 2D canvas fillStyle/strokeStyle). */
   int mini_parse_color(const char *val, float *r, float *g, float *b, float *a);

   /* ---- 2D canvas recording (replayed at the canvas element's render) ----
      The JS bridge records shape commands during requestAnimationFrame;
      render replays them at the canvas's z-position (so 2D content paints
      behind higher-z siblings) and clears. Single active context; 1-frame
      latency (rAF records N, render N+1 replays).                       */
   struct MiniRenderer;
   void mini_2d_reset(void); /* new context frame */
   void mini_2d_begin_path(void);
   void mini_2d_close_path(void);
   void mini_2d_line_to(float x, float y); /* + moveTo */
   void mini_2d_arc(float cx, float cy, float r, float a0, float a1, int ccw);
   void mini_2d_fill(float r, float g, float b, float a);
   void mini_2d_stroke(float r, float g, float b, float a, float w);
   void mini_2d_fill_rect(float x, float y, float w, float h, float cr, float cg, float cb, float ca);
   void mini_2d_clear_rect(float x, float y, float w, float h);
   void mini_2d_stroke_rect(float x, float y, float w, float h,
                            float cr, float cg, float cb, float ca, float lw);
   void mini_2d_save(void);
   void mini_2d_restore(void);
   void mini_2d_translate(float x, float y);
   void mini_2d_scale(float sx, float sy);
   void mini_2d_rotate(float rad);
   /* Stage 3: text / transforms / measure / hit-test */
   void mini_2d_set_font(const char *font_str);
   void mini_2d_fill_text(const char *text, float x, float y, float maxw,
                          float r, float g, float b, float a);
   void mini_2d_stroke_text(const char *text, float x, float y, float maxw,
                            float r, float g, float b, float a);
   void mini_2d_measure_text(const char *text, float *out_w);
   void mini_2d_transform(float a, float b, float c, float d, float e, float f);
   void mini_2d_set_transform(float a, float b, float c, float d, float e, float f);
   void mini_2d_reset_transform(void);
   void mini_2d_is_point_in_path(float x, float y, int *out);
   void mini_2d_get_pen(float *x, float *y);
   void mini_2d_replay(struct MiniRenderer *r, struct MiniNode *canvas);

   static void draw_text_wrapped(MiniRenderer *r, const char *text,
                                 float x, float y, float max_w, float fs,
                                 float cr, float cg, float cb, float ca, int align);

   /* ---- text selection helpers (shared by render + hit-test) -------- */
   /* Collapse runs of ASCII whitespace in `src` into single spaces, mirroring
      the renderer's text-node pre-pass (so selection offsets and the painted
      glyphs use the same string). When is_pre (white-space:pre) the source is
      copied verbatim. dst is NUL-terminated; cap is the buffer size.        */
   void mini_text_collapse(const char *src, char *dst, size_t cap, int is_pre);

   /* Greedy word-wrap line breaks mirroring draw_text_wrapped_ex: fills
      out_start/out_end (byte offsets into `text`) and out_w (pixel width) per
      line, up to max_lines. Returns the line count. \n forces a break.       */
   int mini_text_break_lines(const char *text, float wrap_w, float fs,
                             float ls, int *out_start, int *out_end,
                             float *out_w, int max_lines);

   /* The collapsed text + geometry the renderer uses for a text node, so the
      hit-test and the highlight operate on the exact same string/box as the
      painted glyphs. Fills the outputs; returns the collapsed length.        */
   int mini_dom_text_layout(const struct MiniNode *n, char *collapsed,
                            size_t cap, float *out_fs, float *out_ls,
                            float *out_lh, float *out_draw_x,
                            float *out_draw_y, float *out_wrap_w,
                            int *out_align);

   /* ------------------------------------------------------------------ */
   /* Layout (reduced: block + flexbox). Writes geometry into style.*     */
   /* ------------------------------------------------------------------ */
   void mini_layout_run(MiniDocument *doc, int viewport_w, int viewport_h);

   /* ---- Phase 1: micro HTML5 parser ---------------------------------- */
   /* Parse a raw HTML5 string into the live document tree (appends to
      doc->body). Compact tokenizer: tags/attrs/text, void elements,
      raw-text <script>/<style>, comments, doctype. Not a full spec
      tokenizer, but handles real-world pages enough to host Vue/React. */
   void mini_dom_parse_html(MiniDocument *doc, const char *html);

   /* create a text node from a (data,len) span (parser-internal, exported
      so the self-test can use it) */
   struct MiniNode *mini_node_create_text_n(const char *data, size_t len);

   /* ---- Phase 1.2: micro CSS selector matcher ------------------------- */
   /* Comma-separated simple selectors: tag, .class, #id, *, and compound
      forms (div.x#y, [attr], [attr=val]). No pseudo/nth — kept tiny.
      Returns count of matches written into out[] (capped at max). */
   int mini_dom_query_selector_all(struct MiniDocument *doc, const char *selector,
                                   struct MiniNode **out, int max);
   struct MiniNode *mini_dom_query_selector(struct MiniDocument *doc,
                                            const char *selector);

   /* Apply a raw CSS stylesheet (selector { prop:val; ... } rules) to the
      live tree via the matcher. Last-writer-wins; inline style overrides. */
   void mini_css_apply(struct MiniDocument *doc, const char *css);

   /* Get document title from <title> element */
   const char *mini_doc_get_title(const struct MiniDocument *doc);

   /* Serialize the DOM tree as a CDP DOM.getDocument root object (for the
      Chrome DevTools Elements panel). Writes a null-terminated JSON string. */
   void mini_dom_serialize_cdp(MiniDocument *doc, char *out, size_t cap);

   /* ---- CDP per-node access (Elements panel node-id addressing) -------- */
   /* Assign stable sequential cdp_node_id values to every node in the tree
      (root=1). Idempotent: nodes that already have an id keep it. Returns the
      highest id assigned (== the count of nodes). Call before serializing. */
   int mini_dom_assign_node_ids(MiniDocument *doc);

   /* Resolve a stable node id to its MiniNode (linear scan; fine for the
      modest trees this engine hosts). NULL if not found. */
   struct MiniNode *mini_dom_node_by_id(MiniDocument *doc, int id);

   /* Read a node's stable id (0 if unassigned). */
   int mini_dom_node_id(const struct MiniNode *n);

   /* CDP Node JSON for one node (shallow: node + its immediate children, one
      level). For DOM.describeNode. Writes a null-terminated JSON string. */
   void mini_dom_describe_node(const struct MiniNode *n, char *out, size_t cap);

   /* CDP child-node array JSON for one node's immediate children. For
      DOM.requestChildNodes (we already send the full tree on getDocument,
      so this is mainly for completeness). */
   void mini_dom_node_children_cdp(const struct MiniNode *n, char *out, size_t cap);

   /* CDP BoxModel JSON: content/padding/border/margin quads (each 4 points,
      x1,y1..x4,y4) plus width/height, derived from the node's computed
      layout geometry. For DOM.getBoxModel / CSS.getLayoutMetrics. */
   void mini_dom_box_model(const struct MiniNode *n, char *out, size_t cap);

   /* Computed CSS property pairs (name,value) as a JSON array, from MiniStyle.
      For CSS.getComputedStyle. */
   void mini_dom_computed_style(const struct MiniNode *n, char *out, size_t cap);

   /* The node's inline `style` attribute text (or "" if none). For
      CSS.getInlineStyles. Writes a null-terminated string (NOT JSON). */
   void mini_dom_inline_style(const struct MiniNode *n, char *out, size_t cap);

   /* Serialize a node as HTML. inner=0 -> outerHTML (tag + children + close);
      inner=1 -> innerHTML (children only). Writes a null-terminated string. */
   void mini_dom_outer_html(const struct MiniNode *n, int inner,
                            char *out, size_t cap);

   /* ---- Live DOM mutation events (for the Elements panel) -------------- */
   /* Called from append_child/remove_child/set_attribute when the tree is
      mutated, so the CDP layer can broadcast DOM.childNodeInserted/
      childNodeRemoved/attributeModified. evt is one of those strings. */
   typedef void (*MiniMutationHook)(struct MiniDocument *doc, const char *evt,
                                    struct MiniNode *parent, struct MiniNode *node,
                                    const char *name, const char *value, void *ud);
   void mini_dom_set_mutation_hook(struct MiniDocument *doc, MiniMutationHook hook, void *ud);

   /* ================================================================== */
   /* Phase 2: complete DOM node hierarchy                                */
   /* Document (9) / Element (1) / Text (3) / Comment (8) / Attr (2) /     */
   /* DocumentType (10) / DocumentFragment (11, also ShadowRoot).          */
   /* ================================================================== */
   struct MiniNode *mini_node_create_comment(const char *data);
   struct MiniNode *mini_node_create_document_type(const char *name);
   struct MiniNode *mini_node_create_document_fragment(void);
   struct MiniNode *mini_node_create_attribute(const char *name, const char *value);

   /* W3C tree ops added on top of append/insert/remove (return 0 / child). */
   struct MiniNode *mini_node_clone(struct MiniNode *n, int deep);
   int mini_node_replace_child(struct MiniNode *parent,
                               struct MiniNode *new_child,
                               struct MiniNode *old_child);
   int mini_node_contains(const struct MiniNode *root,
                          const struct MiniNode *target);
   struct MiniNode *mini_node_first_element_child(const struct MiniNode *n);
   struct MiniNode *mini_node_last_element_child(const struct MiniNode *n);
   int mini_node_element_child_count(const struct MiniNode *n);

   /* Live-collection-style queries (fill out[], return count). */
   int mini_dom_get_elements_by_tag_name(struct MiniDocument *doc, const char *tag,
                                         struct MiniNode **out, int max);
   int mini_dom_get_elements_by_class_name(struct MiniDocument *doc,
                                           const char *cls,
                                           struct MiniNode **out, int max);

   /* ================================================================== */
   /* Phase 3: Web Components (Custom Elements + Shadow DOM)             */
   /* ================================================================== */
   /* Attach a shadow root (DocumentFragment-mode node) to an element. The
      shadow tree replaces the element's light-tree children at render time
      unless a <slot> in the shadow tree pulls them in. Returns the root. */
   struct MiniNode *mini_node_attach_shadow(struct MiniNode *host);
   struct MiniNode *mini_node_shadow_root(const struct MiniNode *host);

   /* Serialize the DOM tree as indented HTML (for innerHTML / debugging). */
   void mini_dom_serialize_html(MiniDocument *doc, char *out, size_t cap);

   /* ================================================================== */
   /* Phase 4: dynamic DOM + frame tick + interaction-state hooks         */
   /* ================================================================== */
   /* Replace a node's children by parsing `html` into a fragment. Drives a
      layout dirty mark so the next tick_frame relays out the new subtree.
      Owned by mini_dom.c.                                               */
   void mini_node_set_inner_html(struct MiniNode *n, const char *html);

   /* The per-frame driver: drains dirty marks (relayout / repaint), steps
      CSS transitions (color/opacity interpolation), and clears the 2D
      canvas recording gate. Call once per frame with the wall-clock delta
      in seconds. Layout itself still runs via mini_layout_run — tick_frame
      only flags which subtrees need it (full incremental layout wiring is
      incremental; today it re-lays the whole document when dirty).       */
   void mini_dom_tick_frame(MiniDocument *doc, double delta_time);

   /* Interaction-state entry point for mini_events.c: set/clear the
      :hover/:active/:focus bits on a node (0/1 each, -1 = leave unchanged)
      and mark it dirty for restyle.                                      */
   void mini_node_set_interaction_state(struct MiniNode *n, int hovered,
                                        int active, int focused);
   /* Convenience dirty marker (1 = mark, 0 = leave).                     */
   void mini_node_mark_dirty(struct MiniNode *n, int layout, int paint);

   /* Document (pre-order) comparison: 1 if `a` precedes `b` in the tree
      (a is an ancestor of b, or a's branch is an earlier sibling of b's
      branch under their lowest common ancestor). NULL sorts first. Used by
      text selection to order the anchor/focus endpoints.                  */
   int mini_node_precedes(const struct MiniNode *a, const struct MiniNode *b);

   /* Re-apply the page stylesheet so :hover / :active / :focus rules take effect. */
   void mini_dom_restyle(struct MiniDocument *doc);

   /* Hand the live event state to the render pass so the text-node branch can
      paint the selection highlight. Called by the host each frame before
      mini_dom_render_into. */
   struct MiniEventState;
   void mini_dom_set_render_events(struct MiniEventState *st);

   /* ================================================================== */
   /* Phase 5: Resource Provider & Font Management System                */
   /* ================================================================== */
   typedef struct MiniResource
   {
      const uint8_t *data;
      size_t size;
      int should_free;
   } MiniResource;

   typedef MiniResource (*MiniResourceLoaderCb)(const char *url_or_path, const char *type, void *user_data);

   void mini_dom_set_resource_loader(struct MiniDocument *doc, MiniResourceLoaderCb cb, void *user_data);
   MiniResource mini_dom_load_resource(struct MiniDocument *doc, const char *url_or_path, const char *type);
   void mini_dom_free_resource(MiniResource *res);
   void mini_dom_register_font(struct MiniDocument *doc, const char *family_name, const uint8_t *data, size_t size);
   void mini_2d_move_to(float x, float y);
   int mini_dom_matches_selector(const struct MiniNode *n, const char *selector);
   const char *mini_dom_get_stylesheet(const struct MiniDocument *doc);
#ifdef __cplusplus
}
#endif
#endif /* MINI_DOM_H */

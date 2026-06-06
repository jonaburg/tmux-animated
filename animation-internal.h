/* Shared internals for animation.c and animation-*.c kind-specific files. */

#ifndef TMUX_ANIMATION_INTERNAL_H
#define TMUX_ANIMATION_INTERNAL_H

enum animation_kind {
	ANIM_SLIDE_WINDOW,
	ANIM_PANE_LAYOUT,
	ANIM_SCROLL,
};

enum pane_anim_phase {
	PANE_RESIZE,
	PANE_BORN,
	PANE_DYING,
};

struct pane_anim {
	int			 pane_id;
	enum pane_anim_phase	 phase;

	int			 src_x, src_y, src_w, src_h;
	int			 tgt_x, tgt_y, tgt_w, tgt_h;

	struct screen		*snapshot;
	struct colour_palette	 snapshot_palette;

	struct screen		*backdrop;
	struct colour_palette	 backdrop_palette;
};

struct pane_layout_capture {
	struct window		*w;
	struct pane_anim	*panes;
	size_t			 n;
	size_t			 cap;
	int			 dying_id;
	struct screen		*dying_snapshot;
	struct colour_palette	 dying_palette;
};

enum animation_easing {
	ANIM_EASE_SMOOTHDAMP,
	ANIM_EASE_LINEAR,
	ANIM_EASE_IN_OUT,
};

struct animation {
	struct client		*client;
	enum animation_kind	 kind;
	enum animation_easing	 easing;

	uint64_t		 start_ms;
	uint64_t		 last_ms;
	uint64_t		 duration_ms;
	uint64_t		 tau_ms;
	uint64_t		 frame_interval_ms;

	struct winlink		*src_wl;
	struct winlink		*tgt_wl;
	int			 axis;

	double			 pos;
	double			 vel;
	double			 start_pos;
	double			 target_pos;

	u_int			 sx;
	u_int			 sy;
	u_int			 pane_y0;
	u_int			 pane_h;
	u_int			 pane_x0;
	u_int			 pane_w;

	int			 status_active;
	int			 status_y0;
	int			 src_idx;
	int			 tgt_idx;
	int			 src_row, src_start, src_end;
	int			 tgt_row, tgt_start, tgt_end;

	/* ANIM_SLIDE_WINDOW */
	struct pane_anim	*sw_src_panes;
	size_t			 sw_src_n;
	int			 sw_src_active;

	/* ANIM_PANE_LAYOUT */
	struct window		*pl_window;
	struct pane_anim	*pl_panes;
	size_t			 pl_n;

	/* ANIM_SCROLL */
	struct screen		*scroll_snap;
	struct colour_palette	 scroll_palette;
	double			 scroll_src_off;
	double			 scroll_tgt_off;
	int			 scroll_snap_max_oy;

	struct visible_ranges	 vis;
};

/* Shared helpers (animation.c). */
enum animation_easing	 animation_easing_lookup(struct session *);
double			 animation_ease_cubic_in_out(double);
void			 animation_clone_screen(struct screen *,
			     struct screen *);
uint64_t		 get_timer(void);

/* Overlay callbacks live in animation.c and are used by every begin_*. */
struct visible_ranges	*animation_check_cb(struct client *, void *,
			     u_int, u_int, u_int);
void			 animation_draw_cb(struct client *, void *,
			     struct screen_redraw_ctx *);
int			 animation_key_cb(struct client *, void *,
			     struct key_event *);
void			 animation_free_cb(struct client *, void *);
void			 animation_resize_cb(struct client *, void *);
void			 animation_frame_cb(int, short, void *);

/* Per-kind draw entry points dispatched from animation_draw_cb. */
void			 animation_window_draw(struct client *,
			     struct animation *);
void			 animation_pane_draw(struct client *,
			     struct animation *, double);
void			 animation_scroll_draw(struct client *,
			     struct animation *);

/* Per-kind free entry points called from animation_free_cb. */
void			 animation_window_free(struct animation *);
void			 animation_pane_free(struct animation *);
void			 animation_scroll_free(struct animation *);

#endif

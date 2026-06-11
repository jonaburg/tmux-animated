/* $OpenBSD$ */

/*
 * Pane-layout animations (ANIM_PANE_LAYOUT): split/kill/zoom, plus the
 * single-pane variant used for alt-screen program takeover.
 */

#include <sys/types.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "tmux.h"
#include "animation-internal.h"

static void	animation_free_capture(struct client *);
static struct pane_anim *animation_capture_find_pane(
		    struct pane_layout_capture *, int);
static struct window_pane *animation_find_pane(struct window *, int);
static void	animation_paint_pane_clipped(struct client *, struct screen *,
		    struct colour_palette *, int, int, int, int);
static void	animation_paint_border_cell(struct client *, struct window *,
		    enum pane_lines, struct grid_cell *, int, int, int);
static void	animation_paint_pane_border(struct client *, struct window *,
		    int, int, int, int, int);
static void	animation_begin_alt_screen_one(struct client *,
		    struct window_pane *, struct screen *, int);
static void	animation_alt_screen_broadcast(struct window_pane *,
		    struct screen *, int);

static void
animation_free_capture(struct client *c)
{
	struct pane_layout_capture	*cap = c->animation_capture;

	if (cap == NULL)
		return;
	if (cap->dying_snapshot != NULL) {
		screen_free(cap->dying_snapshot);
		free(cap->dying_snapshot);
	}
	free(cap->panes);
	free(cap);
	c->animation_capture = NULL;
}

void
animation_begin_pane_layout(struct client *c, struct window *w,
    struct window_pane *dying_wp)
{
	struct pane_layout_capture	*cap;
	struct window_pane		*wp;
	size_t				 i;

	if (c == NULL || w == NULL || c->session == NULL)
		return;
	if (!options_get_number(c->session->options, "animation-enable"))
		return;
	if (!options_get_number(c->session->options, "animation-pane-layout"))
		return;
	if (c->tty.sx == 0 || c->tty.sy == 0)
		return;
	if (c->overlay_draw != NULL && c->animation == NULL)
		return;

	if (c->animation_capture != NULL)
		animation_free_capture(c);

	cap = xcalloc(1, sizeof *cap);
	cap->w = w;
	cap->dying_id = (dying_wp != NULL) ? (int)dying_wp->id : -1;

	TAILQ_FOREACH(wp, &w->panes, entry)
		cap->cap++;
	if (cap->cap == 0) {
		free(cap);
		return;
	}
	cap->panes = xcalloc(cap->cap, sizeof *cap->panes);

	i = 0;
	TAILQ_FOREACH(wp, &w->panes, entry) {
		struct pane_anim *pa = &cap->panes[i++];
		pa->pane_id = wp->id;
		pa->src_x = wp->xoff;
		pa->src_y = wp->yoff;
		pa->src_w = wp->sx;
		pa->src_h = wp->sy;
		if (wp->screen != NULL) {
			pa->snapshot = xcalloc(1, sizeof *pa->snapshot);
			animation_clone_screen(pa->snapshot, wp->screen);
			memcpy(&pa->snapshot_palette, &wp->palette,
			    sizeof pa->snapshot_palette);
			pa->snapshot_palette.palette = NULL;
			pa->snapshot_palette.default_palette = NULL;
		}
	}
	cap->n = i;
	(void)dying_wp;

	c->animation_capture = cap;
}

static struct pane_anim *
animation_capture_find_pane(struct pane_layout_capture *cap, int pane_id)
{
	size_t	i;
	for (i = 0; i < cap->n; i++)
		if (cap->panes[i].pane_id == pane_id)
			return (&cap->panes[i]);
	return (NULL);
}

void
animation_commit_pane_layout(struct client *c, struct window *w)
{
	struct pane_layout_capture	*cap;
	struct window_pane		*wp;
	struct animation		*a;
	struct timeval			 tv;
	struct pane_anim		*pa;
	size_t				 i;
	int				 dirty = 0;
	int				 y0, y1;

	if (c == NULL)
		return;
	cap = c->animation_capture;
	if (cap == NULL)
		return;
	if (cap->w != w || c->animation != NULL) {
		animation_free_capture(c);
		return;
	}

	for (i = 0; i < cap->n; i++) {
		struct window_pane	*found = NULL;
		pa = &cap->panes[i];
		TAILQ_FOREACH(wp, &w->panes, entry) {
			if ((int)wp->id == pa->pane_id) {
				found = wp;
				break;
			}
		}
		if (found != NULL) {
			pa->tgt_x = found->xoff;
			pa->tgt_y = found->yoff;
			pa->tgt_w = found->sx;
			pa->tgt_h = found->sy;
			pa->phase = PANE_RESIZE;
		} else {
			pa->phase = PANE_DYING;
			pa->tgt_x = pa->src_x + pa->src_w / 2;
			pa->tgt_y = pa->src_y + pa->src_h / 2;
			pa->tgt_w = 0;
			pa->tgt_h = 0;
		}
		if (pa->src_x != pa->tgt_x || pa->src_y != pa->tgt_y ||
		    pa->src_w != pa->tgt_w || pa->src_h != pa->tgt_h)
			dirty = 1;
	}

	TAILQ_FOREACH(wp, &w->panes, entry) {
		if (animation_capture_find_pane(cap, wp->id) != NULL)
			continue;
		if (cap->n >= cap->cap) {
			cap->cap = cap->cap * 2 + 1;
			cap->panes = xreallocarray(cap->panes, cap->cap,
			    sizeof *cap->panes);
		}
		pa = &cap->panes[cap->n++];
		memset(pa, 0, sizeof *pa);
		pa->pane_id = wp->id;
		pa->phase = PANE_BORN;
		pa->src_x = wp->xoff + wp->sx / 2;
		pa->src_y = wp->yoff + wp->sy / 2;
		pa->src_w = 0;
		pa->src_h = 0;
		pa->tgt_x = wp->xoff;
		pa->tgt_y = wp->yoff;
		pa->tgt_w = wp->sx;
		pa->tgt_h = wp->sy;
		dirty = 1;
	}

	if (!dirty) {
		animation_free_capture(c);
		return;
	}

	a = xcalloc(1, sizeof *a);
	a->client = c;
	a->kind = ANIM_PANE_LAYOUT;
	a->easing = animation_easing_lookup(c->session);
	a->duration_ms = options_get_number(c->session->options,
	    "animation-pane-duration");
	a->frame_interval_ms = options_get_number(c->session->options,
	    "animation-frame-interval");
	a->sx = c->tty.sx;
	a->sy = c->tty.sy;

	{
		int	x0 = a->sx, x1 = 0;

		y0 = a->sy;
		y1 = 0;
		for (i = 0; i < cap->n; i++) {
			pa = &cap->panes[i];
			if (pa->phase == PANE_RESIZE &&
			    pa->src_x == pa->tgt_x &&
			    pa->src_y == pa->tgt_y &&
			    pa->src_w == pa->tgt_w &&
			    pa->src_h == pa->tgt_h)
				continue;
			if (pa->src_y < y0) y0 = pa->src_y;
			if (pa->src_y + pa->src_h > y1)
				y1 = pa->src_y + pa->src_h;
			if (pa->tgt_y < y0) y0 = pa->tgt_y;
			if (pa->tgt_y + pa->tgt_h > y1)
				y1 = pa->tgt_y + pa->tgt_h;
			if (pa->src_x < x0) x0 = pa->src_x;
			if (pa->src_x + pa->src_w > x1)
				x1 = pa->src_x + pa->src_w;
			if (pa->tgt_x < x0) x0 = pa->tgt_x;
			if (pa->tgt_x + pa->tgt_w > x1)
				x1 = pa->tgt_x + pa->tgt_w;
		}
		if (y1 <= y0 || x1 <= x0) {
			free(a);
			animation_free_capture(c);
			return;
		}
		a->pane_y0 = (u_int)y0;
		a->pane_h = (u_int)(y1 - y0);
		a->pane_x0 = (u_int)x0;
		a->pane_w = (u_int)(x1 - x0);
	}

	a->pl_window = w;
	a->pl_panes = cap->panes;
	a->pl_n = cap->n;
	cap->panes = NULL;
	cap->n = 0;

	a->start_ms = get_timer();
	a->last_ms = a->start_ms;

	c->animation = a;

	server_client_set_overlay(c, 0, animation_check_cb, NULL,
	    animation_draw_cb, animation_key_cb, animation_free_cb,
	    animation_resize_cb, a);

	evtimer_set(&c->animation_timer, animation_frame_cb, c);
	tv.tv_sec = 0;
	tv.tv_usec = (long)a->frame_interval_ms * 1000L;
	evtimer_add(&c->animation_timer, &tv);

	animation_free_capture(c);
}

static struct window_pane *
animation_find_pane(struct window *w, int id)
{
	struct window_pane	*wp;

	if (w == NULL)
		return (NULL);
	TAILQ_FOREACH(wp, &w->panes, entry) {
		if ((int)wp->id == id)
			return (wp);
	}
	return (NULL);
}

static void
animation_paint_pane_clipped(struct client *c, struct screen *s,
    struct colour_palette *palette,
    int dst_x, int dst_y, int dst_w, int dst_h)
{
	struct grid_cell	defaults;
	u_int			sy, src_sx, src_sy;
	int			nx;

	if (s == NULL || dst_w <= 0 || dst_h <= 0)
		return;
	memcpy(&defaults, &grid_default_cell, sizeof defaults);

	src_sx = screen_size_x(s);
	src_sy = screen_size_y(s);
	nx = dst_w;
	if (nx > (int)src_sx) nx = (int)src_sx;
	if (dst_x + nx > (int)c->tty.sx) nx = (int)c->tty.sx - dst_x;
	if (nx <= 0)
		return;

	for (sy = 0; sy < (u_int)dst_h && sy < src_sy; sy++) {
		int vy = dst_y + sy;
		if (vy < 0 || vy >= (int)c->tty.sy)
			continue;
		tty_draw_line(&c->tty, s, 0, sy, (u_int)nx,
		    (u_int)dst_x, (u_int)vy, &defaults, palette);
	}
}

static void
animation_paint_border_cell(struct client *c, struct window *w,
    enum pane_lines pane_lines, struct grid_cell *style, int cell_type,
    int x, int y)
{
	struct grid_cell	gc, defaults;
	u_int			statuslines;
	int			statustop, content_top, content_bot;

	if (x < 0 || x >= (int)c->tty.sx || y < 0 || y >= (int)c->tty.sy)
		return;

	statuslines = status_line_size(c);
	statustop = (c->session != NULL && statuslines != 0 &&
	    options_get_number(c->session->options, "status-position") == 0);
	content_top = statustop ? (int)statuslines : 0;
	content_bot = (int)c->tty.sy - (statustop ? 0 : (int)statuslines);
	if (y < content_top || y >= content_bot)
		return;

	memcpy(&gc, style, sizeof gc);
	screen_redraw_border_set(w, NULL, pane_lines, cell_type, &gc);
	memcpy(&defaults, &grid_default_cell, sizeof defaults);

	tty_attributes(&c->tty, &gc, &defaults, NULL, NULL);
	tty_cursor(&c->tty, (u_int)x, (u_int)y);
	tty_cell(&c->tty, &gc, &defaults, NULL, NULL);
}

static void
animation_paint_pane_border(struct client *c, struct window *w, int active,
    int lx, int ly, int lw, int lh)
{
	struct grid_cell	 style;
	struct format_tree	*ft;
	enum pane_lines	 pane_lines;
	int		 top = ly - 1, bot = ly + lh;
	int		 left = lx - 1, right = lx + lw;
	int		 px, py;

	if (lw <= 0 || lh <= 0)
		return;
	if (c->session == NULL || c->session->curw == NULL)
		return;

	pane_lines = options_get_number(w->options, "pane-border-lines");

	memcpy(&style, &grid_default_cell, sizeof style);
	ft = format_create_defaults(NULL, c, c->session, c->session->curw,
	    NULL);
	style_add(&style, w->options,
	    active ? "pane-active-border-style" : "pane-border-style", ft);
	format_free(ft);

	for (px = lx; px < lx + lw; px++) {
		animation_paint_border_cell(c, w, pane_lines, &style,
		    CELL_LEFTRIGHT, px, top);
		animation_paint_border_cell(c, w, pane_lines, &style,
		    CELL_LEFTRIGHT, px, bot);
	}
	for (py = ly; py < ly + lh; py++) {
		animation_paint_border_cell(c, w, pane_lines, &style,
		    CELL_TOPBOTTOM, left, py);
		animation_paint_border_cell(c, w, pane_lines, &style,
		    CELL_TOPBOTTOM, right, py);
	}
	animation_paint_border_cell(c, w, pane_lines, &style, CELL_TOPLEFT,
	    left, top);
	animation_paint_border_cell(c, w, pane_lines, &style, CELL_TOPRIGHT,
	    right, top);
	animation_paint_border_cell(c, w, pane_lines, &style, CELL_BOTTOMLEFT,
	    left, bot);
	animation_paint_border_cell(c, w, pane_lines, &style, CELL_BOTTOMRIGHT,
	    right, bot);
}

void
animation_pane_draw(struct client *c, struct animation *a, double t)
{
	struct pane_anim	*pa;
	struct grid_cell	 fill, defaults;
	struct window_pane	*wp_live;
	int			 lx, ly, lw, lh;
	int			 vy;
	size_t			 i;

	(void)defaults;

	{
		struct grid_cell	cell;
		struct window_pane	*wp;
		memcpy(&cell, &grid_default_cell, sizeof cell);
		wp = (a->pl_window != NULL) ? a->pl_window->active : NULL;
		if (wp != NULL)
			tty_default_colours(&cell, wp);
		memcpy(&fill, &cell, sizeof fill);
		for (vy = a->pane_y0; vy < (int)(a->pane_y0 + a->pane_h);
		    vy++) {
			tty_attributes(&c->tty, &fill, &fill,
			    wp ? &wp->palette : NULL, NULL);
			tty_cursor(&c->tty, a->pane_x0, (u_int)vy);
			tty_repeat_space(&c->tty, a->pane_w);
		}
	}

	for (i = 0; i < a->pl_n; i++) {
		struct window_pane	*wp_b;

		pa = &a->pl_panes[i];
		if (pa->backdrop == NULL)
			continue;
		wp_b = animation_find_pane(a->pl_window, pa->pane_id);
		if (wp_b == NULL)
			continue;
		animation_paint_pane_clipped(c, pa->backdrop,
		    &pa->backdrop_palette, (int)wp_b->xoff, (int)wp_b->yoff,
		    (int)wp_b->sx, (int)wp_b->sy);
	}

	{
		int	active_id = -1;
		size_t	pass;

		if (a->pl_window != NULL && a->pl_window->active != NULL)
			active_id = (int)a->pl_window->active->id;

		for (pass = 0; pass < 2; pass++) {
			for (i = 0; i < a->pl_n; i++) {
				pa = &a->pl_panes[i];
				if (pass == 0 && pa->pane_id == active_id)
					continue;
				if (pass == 1 && pa->pane_id != active_id)
					continue;
				if (pa->phase == PANE_RESIZE &&
				    pa->src_x == pa->tgt_x &&
				    pa->src_y == pa->tgt_y &&
				    pa->src_w == pa->tgt_w &&
				    pa->src_h == pa->tgt_h)
					continue;

				lx = (int)lround(pa->src_x +
				    t * (pa->tgt_x - pa->src_x));
				ly = (int)lround(pa->src_y +
				    t * (pa->tgt_y - pa->src_y));
				lw = (int)lround(pa->src_w +
				    t * (pa->tgt_w - pa->src_w));
				lh = (int)lround(pa->src_h +
				    t * (pa->tgt_h - pa->src_h));

				if (c->session != NULL &&
				    options_get_number(c->session->options,
				    "animation-pane-borders"))
					animation_paint_pane_border(c,
					    a->pl_window,
					    pa->pane_id == active_id,
					    lx, ly, lw, lh);

				if (pa->phase == PANE_DYING) {
					animation_paint_pane_clipped(c,
					    pa->snapshot,
					    &pa->snapshot_palette,
					    lx, ly, lw, lh);
				} else {
					struct screen	*s_use = NULL;
					struct colour_palette *pal_use = NULL;

					wp_live = animation_find_pane(
					    a->pl_window, pa->pane_id);
					if (wp_live != NULL &&
					    wp_live->screen != NULL) {
						s_use = wp_live->screen;
						pal_use = &wp_live->palette;
					}
					if (s_use == NULL &&
					    pa->snapshot != NULL) {
						s_use = pa->snapshot;
						pal_use = &pa->snapshot_palette;
					}
					if (s_use == NULL)
						continue;
					animation_paint_pane_clipped(c,
					    s_use, pal_use, lx, ly, lw, lh);
				}
			}
		}
	}
}

void
animation_pane_free(struct animation *a)
{
	size_t	i;

	for (i = 0; i < a->pl_n; i++) {
		if (a->pl_panes[i].snapshot != NULL) {
			screen_free(a->pl_panes[i].snapshot);
			free(a->pl_panes[i].snapshot);
		}
		if (a->pl_panes[i].backdrop != NULL) {
			screen_free(a->pl_panes[i].backdrop);
			free(a->pl_panes[i].backdrop);
		}
	}
	free(a->pl_panes);
}

void
animation_window_pane_layout_begin(struct window *w,
    struct window_pane *dying_wp)
{
	struct client	*c;

	TAILQ_FOREACH(c, &clients, entry) {
		if (c->session == NULL || c->session->curw == NULL)
			continue;
		if (c->session->curw->window != w)
			continue;
		animation_begin_pane_layout(c, w, dying_wp);
	}
}

void
animation_window_pane_layout_cancel(struct window *w)
{
	struct client	*c;

	TAILQ_FOREACH(c, &clients, entry) {
		struct pane_layout_capture	*cap = c->animation_capture;
		if (cap != NULL && cap->w == w)
			animation_free_capture(c);
	}
}

void
animation_window_pane_layout_commit(struct window *w)
{
	struct client	*c;

	TAILQ_FOREACH(c, &clients, entry) {
		if (c->session == NULL || c->session->curw == NULL)
			continue;
		if (c->session->curw->window != w)
			continue;
		animation_commit_pane_layout(c, w);
	}
}

void
animation_capture_dying_pane(__unused struct window_pane *wp)
{
}

/* ---- alt-screen takeover ---- */

static void
animation_begin_alt_screen_one(struct client *c, struct window_pane *wp,
    struct screen *snap, int entering)
{
	struct animation	*a;
	struct pane_anim	*pa;
	struct timeval		 tv;

	if (c == NULL || c->session == NULL || wp == NULL || snap == NULL)
		goto drop;
	if (!options_get_number(c->session->options, "animation-enable"))
		goto drop;
	if (!options_get_number(c->session->options, "animation-alt-screen"))
		goto drop;
	if (c->tty.sx == 0 || c->tty.sy == 0)
		goto drop;
	if (c->overlay_draw != NULL && c->animation == NULL)
		goto drop;
	if (c->animation != NULL)
		goto drop;
	if (wp->sx == 0 || wp->sy == 0)
		goto drop;

	a = xcalloc(1, sizeof *a);
	a->client = c;
	a->kind = ANIM_PANE_LAYOUT;
	a->easing = animation_easing_lookup(c->session);
	a->duration_ms = options_get_number(c->session->options,
	    "animation-pane-duration");
	a->frame_interval_ms = options_get_number(c->session->options,
	    "animation-frame-interval");
	a->sx = c->tty.sx;
	a->sy = c->tty.sy;
	a->pane_y0 = wp->yoff;
	a->pane_h = wp->sy;
	a->pane_x0 = wp->xoff;
	a->pane_w = wp->sx;
	a->pl_window = wp->window;
	a->pl_n = 1;
	a->pl_panes = xcalloc(1, sizeof *a->pl_panes);
	pa = &a->pl_panes[0];
	pa->pane_id = wp->id;

	if (entering) {
		pa->phase = PANE_BORN;
		pa->src_x = wp->xoff + wp->sx / 2;
		pa->src_y = wp->yoff + wp->sy / 2;
		pa->src_w = 0;
		pa->src_h = 0;
		pa->tgt_x = wp->xoff;
		pa->tgt_y = wp->yoff;
		pa->tgt_w = wp->sx;
		pa->tgt_h = wp->sy;
		pa->backdrop = snap;
		memcpy(&pa->backdrop_palette, &wp->palette,
		    sizeof pa->backdrop_palette);
		pa->backdrop_palette.palette = NULL;
		pa->backdrop_palette.default_palette = NULL;
	} else {
		pa->phase = PANE_DYING;
		pa->src_x = wp->xoff;
		pa->src_y = wp->yoff;
		pa->src_w = wp->sx;
		pa->src_h = wp->sy;
		pa->tgt_x = wp->xoff + wp->sx / 2;
		pa->tgt_y = wp->yoff + wp->sy / 2;
		pa->tgt_w = 0;
		pa->tgt_h = 0;
		pa->snapshot = snap;
		memcpy(&pa->snapshot_palette, &wp->palette,
		    sizeof pa->snapshot_palette);
		pa->snapshot_palette.palette = NULL;
		pa->snapshot_palette.default_palette = NULL;
		if (wp->screen != NULL) {
			pa->backdrop = xcalloc(1, sizeof *pa->backdrop);
			animation_clone_screen(pa->backdrop, wp->screen);
			memcpy(&pa->backdrop_palette, &wp->palette,
			    sizeof pa->backdrop_palette);
			pa->backdrop_palette.palette = NULL;
			pa->backdrop_palette.default_palette = NULL;
		}
	}

	a->start_ms = get_timer();
	a->last_ms = a->start_ms;
	c->animation = a;

	server_client_set_overlay(c, 0, animation_check_cb, NULL,
	    animation_draw_cb, animation_key_cb, animation_free_cb,
	    animation_resize_cb, a);

	evtimer_set(&c->animation_timer, animation_frame_cb, c);
	tv.tv_sec = 0;
	tv.tv_usec = (long)a->frame_interval_ms * 1000L;
	evtimer_add(&c->animation_timer, &tv);
	return;

drop:
	if (snap != NULL) {
		screen_free(snap);
		free(snap);
	}
}

static void
animation_alt_screen_broadcast(struct window_pane *wp, struct screen *snap,
    int entering)
{
	struct client	*c;
	int		 placed = 0;

	if (wp == NULL || wp->window == NULL || snap == NULL) {
		if (snap != NULL) {
			screen_free(snap);
			free(snap);
		}
		return;
	}

	TAILQ_FOREACH(c, &clients, entry) {
		struct screen	*per;

		if (c->session == NULL || c->session->curw == NULL)
			continue;
		if (c->session->curw->window != wp->window)
			continue;

		if (!placed) {
			per = snap;
			placed = 1;
		} else {
			per = xcalloc(1, sizeof *per);
			animation_clone_screen(per, snap);
		}
		animation_begin_alt_screen_one(c, wp, per, entering);
	}

	if (!placed) {
		screen_free(snap);
		free(snap);
	}
}

void
animation_alt_screen_enter(struct window_pane *wp, struct screen *snap)
{
	animation_alt_screen_broadcast(wp, snap, 1);
}

void
animation_alt_screen_exit(struct window_pane *wp, struct screen *snap)
{
	animation_alt_screen_broadcast(wp, snap, 0);
}

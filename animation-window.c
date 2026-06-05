/* $OpenBSD$ */

/* Window-switch slide animation (ANIM_SLIDE_WINDOW). */

#include <sys/types.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "tmux.h"
#include "animation-internal.h"

static void	animation_paint_window(struct client *, struct window *, int,
		    u_int, u_int, u_int);
static struct window_pane *animation_window_default_cell(struct window *,
		    struct grid_cell *);
static void	animation_scan_status(struct client *, struct animation *);
static void	animation_paint_status_copy_cell(struct client *, u_int, u_int,
		    u_int, u_int);
static void	animation_paint_status_cell(struct client *, u_int, u_int,
		    u_int, u_int, const struct grid_cell *);
static void	animation_draw_status(struct client *, struct animation *,
		    double);

void
animation_begin_window_switch(struct client *c, struct winlink *src,
    struct winlink *tgt)
{
	struct animation	*a;
	struct session		*s;
	int			 mode;
	uint64_t		 now;
	struct timeval		 tv;

	if (c == NULL || src == NULL || tgt == NULL || src == tgt)
		return;
	if ((s = c->session) == NULL)
		return;

	if (!options_get_number(s->options, "animation-enable"))
		return;
	mode = options_get_number(s->options, "animation-window-switch");
	if (mode == 0)
		return;

	if (c->overlay_draw != NULL && c->animation == NULL)
		return;

	if (c->animation != NULL) {
		animation_retarget(c, tgt);
		return;
	}

	a = xcalloc(1, sizeof *a);
	a->client = c;
	a->kind = ANIM_SLIDE_WINDOW;
	a->easing = animation_easing_lookup(s);
	a->duration_ms = options_get_number(s->options, "animation-duration");
	a->tau_ms = options_get_number(s->options, "animation-tau");
	a->frame_interval_ms =
	    options_get_number(s->options, "animation-frame-interval");

	a->src_wl = src;
	a->tgt_wl = tgt;
	a->axis = (tgt->idx > src->idx) ? +1 : -1;

	a->sx = c->tty.sx;
	a->sy = c->tty.sy;
	if (a->sx == 0 || a->sy == 0) {
		free(a);
		return;
	}

	{
		struct window_pane	*wp;
		u_int			 y0 = a->sy;
		u_int			 y1 = 0;
		int			 any = 0;

		TAILQ_FOREACH(wp, &src->window->panes, entry) {
			if (wp->yoff >= 0 && (u_int)wp->yoff < y0)
				y0 = (u_int)wp->yoff;
			if (wp->yoff >= 0 &&
			    (u_int)wp->yoff + wp->sy > y1)
				y1 = (u_int)wp->yoff + wp->sy;
			any = 1;
		}
		TAILQ_FOREACH(wp, &tgt->window->panes, entry) {
			if (wp->yoff >= 0 && (u_int)wp->yoff < y0)
				y0 = (u_int)wp->yoff;
			if (wp->yoff >= 0 &&
			    (u_int)wp->yoff + wp->sy > y1)
				y1 = (u_int)wp->yoff + wp->sy;
			any = 1;
		}
		if (!any || y1 <= y0) {
			free(a);
			return;
		}
		a->pane_y0 = y0;
		a->pane_h = y1 - y0;
		a->pane_x0 = 0;
		a->pane_w = a->sx;
	}

	a->target_pos = 0;
	a->start_pos = (double)a->sx * a->axis;
	a->pos = a->start_pos;
	a->vel = 0;

	a->src_idx = src->idx;
	a->tgt_idx = tgt->idx;
	animation_scan_status(c, a);

	now = get_timer();
	a->start_ms = now;
	a->last_ms = now;

	c->animation = a;

	server_client_set_overlay(c, 0, animation_check_cb, NULL,
	    animation_draw_cb, animation_key_cb, animation_free_cb,
	    animation_resize_cb, a);

	evtimer_set(&c->animation_timer, animation_frame_cb, c);
	tv.tv_sec = 0;
	tv.tv_usec = (long)a->frame_interval_ms * 1000L;
	evtimer_add(&c->animation_timer, &tv);
}

void
animation_retarget(struct client *c, struct winlink *new_tgt)
{
	struct animation	*a;
	struct winlink		*new_src;
	uint64_t		 now;

	if (c == NULL || (a = c->animation) == NULL || new_tgt == NULL)
		return;
	if (new_tgt == a->tgt_wl)
		return;
	new_src = a->tgt_wl;
	if (new_tgt == new_src)
		return;

	now = get_timer();

	a->src_wl = new_src;
	a->tgt_wl = new_tgt;
	a->axis = (new_tgt->idx > new_src->idx) ? +1 : -1;

	a->start_pos = (double)a->sx * a->axis;
	a->pos = a->start_pos;
	a->target_pos = 0;
	a->vel = 0;
	a->start_ms = now;
	a->last_ms = now;

	a->src_idx = new_src->idx;
	a->tgt_idx = new_tgt->idx;
	animation_scan_status(c, a);
}

static void
animation_paint_window(struct client *c, struct window *w, int dx,
    u_int view_w, u_int view_y0, u_int view_h)
{
	struct window_pane	*wp;
	struct grid_cell	 defaults;
	int			 dst_x, src_px, nx;
	u_int			 sy, vy;

	if (w == NULL)
		return;

	memcpy(&defaults, &grid_default_cell, sizeof defaults);

	TAILQ_FOREACH(wp, &w->panes, entry) {
		if (wp->screen == NULL)
			continue;

		dst_x = (int)wp->xoff + dx;
		src_px = 0;
		nx = (int)wp->sx;
		if (dst_x < 0) { src_px = -dst_x; nx -= src_px; dst_x = 0; }
		if (dst_x + nx > (int)view_w) nx = (int)view_w - dst_x;
		if (nx <= 0)
			continue;

		for (sy = 0; sy < wp->sy; sy++) {
			vy = wp->yoff + sy;
			if (vy < view_y0 || vy >= view_y0 + view_h)
				continue;
			tty_draw_line(&c->tty, wp->screen,
			    (u_int)src_px, sy, (u_int)nx,
			    (u_int)dst_x, vy,
			    &defaults, &wp->palette);
		}
	}
}

static struct window_pane *
animation_window_default_cell(struct window *w, struct grid_cell *out)
{
	struct window_pane	*wp;

	memcpy(out, &grid_default_cell, sizeof *out);
	if (w == NULL || (wp = w->active) == NULL)
		return (NULL);
	tty_default_colours(out, wp);
	return (wp);
}

static void
animation_scan_status(struct client *c, struct animation *a)
{
	struct status_line	*sl = &c->status;
	struct style_range	*sr;
	int			 y, lines;
	int			 found_src = 0, found_tgt = 0;

	a->status_active = 0;
	if (!options_get_number(c->session->options,
	    "animation-status-highlight"))
		return;
	lines = status_line_size(c);
	if (lines <= 0)
		return;
	a->status_y0 = status_at_line(c);
	if (a->status_y0 < 0)
		return;

	for (y = 0; y < lines; y++) {
		TAILQ_FOREACH(sr, &sl->entries[y].ranges, entry) {
			if (sr->type != STYLE_RANGE_WINDOW)
				continue;
			if (!found_src &&
			    (int)sr->argument == a->src_idx) {
				a->src_row = y;
				a->src_start = sr->start;
				a->src_end = sr->end;
				found_src = 1;
			}
			if (!found_tgt &&
			    (int)sr->argument == a->tgt_idx) {
				a->tgt_row = y;
				a->tgt_start = sr->start;
				a->tgt_end = sr->end;
				found_tgt = 1;
			}
		}
	}
	if (found_src && found_tgt)
		a->status_active = 1;
}

static void
animation_paint_status_copy_cell(struct client *c, u_int sx, u_int sy,
    u_int tx, u_int ty)
{
	struct grid_cell	gc, defaults;

	grid_view_get_cell(c->status.screen.grid, sx, sy, &gc);
	if (gc.flags & GRID_FLAG_PADDING)
		return;
	memcpy(&defaults, &grid_default_cell, sizeof defaults);
	tty_attributes(&c->tty, &gc, &defaults, NULL, NULL);
	tty_cursor(&c->tty, tx, ty);
	if (gc.data.size == 0)
		tty_putc(&c->tty, ' ');
	else
		tty_putn(&c->tty, gc.data.data, gc.data.size, gc.data.width);
}

static void
animation_paint_status_cell(struct client *c, u_int sx, u_int sy, u_int tx,
    u_int ty, const struct grid_cell *style)
{
	struct grid_cell	gc, defaults;

	grid_view_get_cell(c->status.screen.grid, sx, sy, &gc);
	if (gc.flags & GRID_FLAG_PADDING)
		return;
	gc.fg = style->fg;
	gc.bg = style->bg;
	gc.attr = style->attr;
	memcpy(&defaults, &grid_default_cell, sizeof defaults);
	tty_attributes(&c->tty, &gc, &defaults, NULL, NULL);
	tty_cursor(&c->tty, tx, ty);
	if (gc.data.size == 0)
		tty_putc(&c->tty, ' ');
	else
		tty_putn(&c->tty, gc.data.data, gc.data.size, gc.data.width);
}

static void
animation_draw_status(struct client *c, struct animation *a, double t)
{
	struct grid_cell	inactive_style;
	double			 lstart;
	int			 lerp_x, lerp_w, tgt_w;
	int			 row, ty, x, i;

	if (!a->status_active)
		return;
	if (a->src_row != a->tgt_row)
		return;
	row = a->src_row;
	ty = a->status_y0 + row;

	tgt_w = a->tgt_end - a->tgt_start;
	if (tgt_w <= 0)
		return;
	lerp_w = tgt_w;
	lstart = (double)a->src_start + t *
	    ((double)a->tgt_start - a->src_start);
	lerp_x = (int)lround(lstart);

	{
		u_int imid = a->src_start + (a->src_end - a->src_start) / 2;
		if ((int)imid >= lerp_x && (int)imid < lerp_x + lerp_w)
			imid = a->src_end;
		grid_view_get_cell(c->status.screen.grid, imid, row,
		    &inactive_style);
	}

	for (x = a->tgt_start; x < a->tgt_end; x++) {
		if (x >= lerp_x && x < lerp_x + lerp_w)
			continue;
		animation_paint_status_cell(c, (u_int)x, (u_int)row,
		    (u_int)x, (u_int)ty, &inactive_style);
	}

	for (i = 0; i < lerp_w; i++) {
		int dst_x = lerp_x + i;
		int src_col = a->tgt_start + i;

		if (dst_x < 0 || dst_x >= (int)c->tty.sx)
			continue;
		if (src_col < 0 || src_col >= a->tgt_end)
			continue;
		animation_paint_status_copy_cell(c, (u_int)src_col,
		    (u_int)row, (u_int)dst_x, (u_int)ty);
	}
}

void
animation_window_draw(struct client *c, struct animation *a)
{
	int			 src_dx, tgt_dx;
	struct window_pane	*src_wp, *tgt_wp;
	struct grid_cell	 src_cell, tgt_cell;
	int			 src_l, src_r, tgt_l, tgt_r;
	u_int			 vy;

	if (a->src_wl == NULL || a->tgt_wl == NULL)
		return;
	if (a->src_wl->window == NULL || a->tgt_wl->window == NULL)
		return;

	tgt_dx = (int)lround(a->pos);
	src_dx = (int)lround(a->pos - (double)a->sx * a->axis);

	src_wp = animation_window_default_cell(a->src_wl->window, &src_cell);
	tgt_wp = animation_window_default_cell(a->tgt_wl->window, &tgt_cell);

	src_l = src_dx > 0 ? src_dx : 0;
	src_r = src_dx + (int)a->sx;
	if (src_r > (int)a->sx) src_r = (int)a->sx;
	tgt_l = tgt_dx > 0 ? tgt_dx : 0;
	tgt_r = tgt_dx + (int)a->sx;
	if (tgt_r > (int)a->sx) tgt_r = (int)a->sx;

	for (vy = a->pane_y0; vy < a->pane_y0 + a->pane_h; vy++) {
		if (src_wp != NULL && src_r > src_l) {
			tty_attributes(&c->tty, &src_cell, &src_cell,
			    &src_wp->palette, NULL);
			tty_cursor(&c->tty, (u_int)src_l, vy);
			tty_repeat_space(&c->tty, (u_int)(src_r - src_l));
		}
		if (tgt_wp != NULL && tgt_r > tgt_l) {
			tty_attributes(&c->tty, &tgt_cell, &tgt_cell,
			    &tgt_wp->palette, NULL);
			tty_cursor(&c->tty, (u_int)tgt_l, vy);
			tty_repeat_space(&c->tty, (u_int)(tgt_r - tgt_l));
		}
	}

	animation_paint_window(c, a->src_wl->window, src_dx, a->sx,
	    a->pane_y0, a->pane_h);
	animation_paint_window(c, a->tgt_wl->window, tgt_dx, a->sx,
	    a->pane_y0, a->pane_h);

	if (a->status_active && a->duration_ms > 0) {
		uint64_t now = get_timer();
		double	 t = (double)(now - a->start_ms) /
		    (double)a->duration_ms;
		if (t < 0) t = 0;
		if (t > 1) t = 1;
		animation_draw_status(c, a, t);
	}
}

void
animation_window_free(__unused struct animation *a)
{
}

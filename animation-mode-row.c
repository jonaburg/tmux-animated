/* $OpenBSD$ */

/* Mode-tree active-row slide animation (ANIM_MODE_ROW). */

#include <sys/types.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "tmux.h"
#include "animation-internal.h"

static void
animation_mode_row_paint_inactive(struct client *c, struct window_pane *wp,
    u_int visible_y, u_int width)
{
	struct grid_cell	 gc, defaults;
	u_int			 col, ty;

	if (visible_y >= screen_size_y(wp->screen))
		return;
	ty = wp->yoff + visible_y;
	if (ty >= c->tty.sy)
		return;
	memcpy(&defaults, &grid_default_cell, sizeof defaults);

	for (col = 0; col < width && col < wp->sx; col++) {
		grid_view_get_cell(wp->screen->grid, col, visible_y, &gc);
		if (gc.flags & GRID_FLAG_PADDING)
			continue;
		gc.fg = defaults.fg;
		gc.bg = defaults.bg;
		gc.attr = defaults.attr;
		tty_attributes(&c->tty, &gc, &defaults, &wp->palette, NULL);
		tty_cursor(&c->tty, wp->xoff + col, ty);
		if (gc.data.size == 0)
			tty_putc(&c->tty, ' ');
		else
			tty_putn(&c->tty, gc.data.data, gc.data.size,
			    gc.data.width);
	}
}

static void
animation_mode_row_paint_bar(struct client *c, struct window_pane *wp,
    struct grid_cell *cells, u_int n, u_int dst_visible_y)
{
	struct grid_cell	 defaults;
	u_int			 col, ty;

	ty = wp->yoff + dst_visible_y;
	if (ty >= c->tty.sy)
		return;
	memcpy(&defaults, &grid_default_cell, sizeof defaults);

	for (col = 0; col < n && col < wp->sx; col++) {
		struct grid_cell	*gc = &cells[col];
		if (gc->flags & GRID_FLAG_PADDING)
			continue;
		tty_attributes(&c->tty, gc, &defaults, &wp->palette, NULL);
		tty_cursor(&c->tty, wp->xoff + col, ty);
		if (gc->data.size == 0)
			tty_putc(&c->tty, ' ');
		else
			tty_putn(&c->tty, gc->data.data, gc->data.size,
			    gc->data.width);
	}
}

void
animation_begin_mode_row(struct client *c, struct window_pane *wp,
    int src_visible_y, int tgt_visible_y, u_int width)
{
	struct animation	*a;
	struct timeval		 tv;

	if (c == NULL || c->session == NULL || wp == NULL)
		return;
	if (abs(tgt_visible_y - src_visible_y) < 2)
		return;
	if (!options_get_number(c->session->options, "animation-enable"))
		return;
	if (!options_get_number(c->session->options, "animation-mode-row"))
		return;
	if (c->tty.sx == 0 || c->tty.sy == 0)
		return;
	if (c->overlay_draw != NULL && c->animation == NULL)
		return;
	if (c->animation != NULL)
		return;
	if (wp->sx == 0 || wp->sy == 0)
		return;
	if (src_visible_y < 0 || tgt_visible_y < 0)
		return;
	if ((u_int)src_visible_y >= wp->sy || (u_int)tgt_visible_y >= wp->sy)
		return;

	a = xcalloc(1, sizeof *a);
	a->client = c;
	a->kind = ANIM_MODE_ROW;
	a->easing = ANIM_EASE_IN_OUT;
	a->duration_ms = options_get_number(c->session->options,
	    "animation-mode-row-duration");
	a->frame_interval_ms = options_get_number(c->session->options,
	    "animation-frame-interval");
	a->sx = c->tty.sx;
	a->sy = c->tty.sy;

	a->modrow_wp = wp;
	a->modrow_src_y = src_visible_y;
	a->modrow_tgt_y = tgt_visible_y;
	a->modrow_width = width;
	{
		u_int n = width;
		u_int i;
		if (n > wp->sx) n = wp->sx;
		if (n > screen_size_x(wp->screen)) n = screen_size_x(wp->screen);
		a->modrow_src_cells = xcalloc(n, sizeof *a->modrow_src_cells);
		a->modrow_src_n = n;
		for (i = 0; i < n; i++) {
			grid_view_get_cell(wp->screen->grid, i,
			    (u_int)src_visible_y, &a->modrow_src_cells[i]);
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
}

void
animation_mode_row_draw(struct client *c, struct animation *a, double t)
{
	struct window_pane	*wp = a->modrow_wp;
	double			 lerp_yd;
	int			 lerp_y, lo, hi, y;

	if (wp == NULL || wp->window == NULL)
		return;
	if (a->modrow_tgt_y >= (int)wp->sy || a->modrow_src_y >= (int)wp->sy)
		return;

	lerp_yd = (double)a->modrow_src_y +
	    t * ((double)a->modrow_tgt_y - a->modrow_src_y);
	lerp_y = (int)lround(lerp_yd);

	lo = a->modrow_src_y < a->modrow_tgt_y ?
	    a->modrow_src_y : a->modrow_tgt_y;
	hi = a->modrow_src_y > a->modrow_tgt_y ?
	    a->modrow_src_y : a->modrow_tgt_y;

	for (y = lo; y <= hi; y++) {
		if (y == lerp_y)
			continue;
		animation_mode_row_paint_inactive(c, wp, (u_int)y,
		    a->modrow_width);
	}
	if (a->modrow_src_cells != NULL) {
		animation_mode_row_paint_bar(c, wp, a->modrow_src_cells,
		    a->modrow_src_n, (u_int)lerp_y);
	}
}

void
animation_mode_row_free(struct animation *a)
{
	free(a->modrow_src_cells);
	a->modrow_src_cells = NULL;
	a->modrow_src_n = 0;
	a->modrow_wp = NULL;
}

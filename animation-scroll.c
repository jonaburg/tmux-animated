/* $OpenBSD$ */

/* Copy-mode scroll animation (ANIM_SCROLL). */

#include <sys/types.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "tmux.h"
#include "animation-internal.h"

void
animation_begin_scroll(struct client *c, struct window_pane *wp,
    struct screen *src, int old_oy, int new_oy)
{
	struct animation	*a;
	struct timeval		 tv;
	u_int			 hsize, snap_h, snap_start;
	int			 max_oy, min_oy;

	if (c == NULL || c->session == NULL || wp == NULL || src == NULL)
		return;
	if (old_oy == new_oy)
		return;
	if (!options_get_number(c->session->options, "animation-enable"))
		return;
	if (!options_get_number(c->session->options, "animation-scroll"))
		return;
	if (c->tty.sx == 0 || c->tty.sy == 0)
		return;
	if (c->overlay_draw != NULL && c->animation == NULL)
		return;

	/*
	 * In-flight scroll: replace old_oy with the current visual oy so the
	 * new animation continues from where the eye is, with no snap-back.
	 */
	if (c->animation != NULL && c->animation->kind == ANIM_SCROLL) {
		struct animation	*prev = c->animation;
		double			 t = 0, cur_off, visual_oy;

		if (prev->duration_ms > 0) {
			uint64_t now = get_timer();
			t = (double)(now - prev->start_ms) /
			    (double)prev->duration_ms;
		}
		if (t < 0) t = 0;
		if (t > 1) t = 1;
		if (prev->easing == ANIM_EASE_IN_OUT)
			t = animation_ease_cubic_in_out(t);
		cur_off = prev->scroll_src_off + t *
		    (prev->scroll_tgt_off - prev->scroll_src_off);
		visual_oy = (double)prev->scroll_snap_max_oy - cur_off;
		old_oy = (int)lround(visual_oy);
		animation_cancel(c);
	} else if (c->animation != NULL) {
		animation_cancel(c);
	}
	if (wp->sx == 0 || wp->sy == 0)
		return;

	hsize = screen_hsize(src);
	max_oy = old_oy > new_oy ? old_oy : new_oy;
	min_oy = old_oy < new_oy ? old_oy : new_oy;
	if (max_oy < 0 || (u_int)max_oy > hsize)
		return;

	snap_h = wp->sy + (u_int)(max_oy - min_oy);
	snap_start = hsize - (u_int)max_oy;

	a = xcalloc(1, sizeof *a);
	a->client = c;
	a->kind = ANIM_SCROLL;
	a->easing = ANIM_EASE_IN_OUT;
	a->duration_ms = options_get_number(c->session->options,
	    "animation-scroll-duration");
	a->frame_interval_ms = options_get_number(c->session->options,
	    "animation-frame-interval");
	a->sx = c->tty.sx;
	a->sy = c->tty.sy;
	a->pane_x0 = wp->xoff;
	a->pane_w = wp->sx;
	a->pane_y0 = wp->yoff;
	a->pane_h = wp->sy;

	a->scroll_snap = xcalloc(1, sizeof *a->scroll_snap);
	screen_init(a->scroll_snap, wp->sx, snap_h, 0);
	grid_duplicate_lines(a->scroll_snap->grid, 0, src->grid, snap_start,
	    snap_h);
	memcpy(&a->scroll_palette, &wp->palette, sizeof a->scroll_palette);
	a->scroll_palette.palette = NULL;
	a->scroll_palette.default_palette = NULL;

	a->scroll_src_off = (double)(max_oy - old_oy);
	a->scroll_tgt_off = (double)(max_oy - new_oy);
	a->scroll_snap_max_oy = max_oy;

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
animation_scroll_draw(struct client *c, struct animation *a)
{
	double			 t, off;
	struct grid_cell	 defaults;
	int			 row_off;
	u_int			 y;

	if (a->duration_ms == 0)
		t = 1;
	else {
		uint64_t now = get_timer();
		t = (double)(now - a->start_ms) / (double)a->duration_ms;
	}
	if (t < 0) t = 0;
	if (t > 1) t = 1;
	if (a->easing == ANIM_EASE_IN_OUT)
		t = animation_ease_cubic_in_out(t);

	off = a->scroll_src_off + t *
	    (a->scroll_tgt_off - a->scroll_src_off);
	row_off = (int)lround(off);
	memcpy(&defaults, &grid_default_cell, sizeof defaults);

	for (y = 0; y < a->pane_h; y++) {
		int src_y = row_off + (int)y;
		if (src_y < 0 ||
		    src_y >= (int)screen_size_y(a->scroll_snap))
			continue;
		tty_draw_line(&c->tty, a->scroll_snap, 0,
		    (u_int)src_y, a->pane_w,
		    a->pane_x0, a->pane_y0 + y,
		    &defaults, &a->scroll_palette);
	}
}

void
animation_scroll_free(struct animation *a)
{
	if (a->scroll_snap != NULL) {
		screen_free(a->scroll_snap);
		free(a->scroll_snap);
	}
}

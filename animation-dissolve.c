/* $OpenBSD$ */

/* Screen dissolve animation (ANIM_SCREEN_DISSOLVE). */

#include <sys/types.h>

#include <stdlib.h>
#include <string.h>

#include "tmux.h"
#include "animation-internal.h"

static const unsigned char bayer8[8][8] = {
	{ 0, 32,  8, 40,  2, 34, 10, 42 },
	{ 48, 16, 56, 24, 50, 18, 58, 26 },
	{ 12, 44,  4, 36, 14, 46,  6, 38 },
	{ 60, 28, 52, 20, 62, 30, 54, 22 },
	{  3, 35, 11, 43,  1, 33,  9, 41 },
	{ 51, 19, 59, 27, 49, 17, 57, 25 },
	{ 15, 47,  7, 39, 13, 45,  5, 37 },
	{ 63, 31, 55, 23, 61, 29, 53, 21 },
};

static double
dissolve_threshold(struct animation *a, u_int col, u_int row)
{
	if (a->dissolve_style == 1) {
		uint32_t h = (uint32_t)(col * 374761393u + row * 668265263u);
		h = (h ^ (h >> 13)) * 1274126177u;
		h ^= h >> 16;
		return ((double)(h & 0xffffff) / (double)0x1000000);
	}
	return (((double)bayer8[row & 7][col & 7] + 0.5) / 64.0);
}

void
animation_begin_screen_dissolve(struct client *c, struct window_pane *wp,
    struct screen *old_snap, struct screen *new_snap)
{
	struct animation	*a;
	struct timeval		 tv;

	if (c == NULL || c->session == NULL || wp == NULL ||
	    old_snap == NULL || new_snap == NULL)
		goto drop;
	if (!options_get_number(c->session->options, "animation-enable"))
		goto drop;
	if (!options_get_number(c->session->options, "animation-mode-enter"))
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
	a->kind = ANIM_SCREEN_DISSOLVE;
	a->easing = ANIM_EASE_LINEAR;
	a->duration_ms = options_get_number(c->session->options,
	    "animation-mode-enter-duration");
	a->frame_interval_ms = options_get_number(c->session->options,
	    "animation-frame-interval");
	a->sx = c->tty.sx;
	a->sy = c->tty.sy;
	a->pane_x0 = wp->xoff;
	a->pane_w = wp->sx;
	a->pane_y0 = wp->yoff;
	a->pane_h = wp->sy;

	a->dissolve_wp = wp;
	a->dissolve_old = old_snap;
	a->dissolve_new = new_snap;
	memcpy(&a->dissolve_palette, &wp->palette, sizeof a->dissolve_palette);
	a->dissolve_palette.palette = NULL;
	a->dissolve_palette.default_palette = NULL;
	a->dissolve_style = options_get_number(c->session->options,
	    "animation-mode-enter-style");

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
	if (old_snap != NULL) {
		screen_free(old_snap);
		free(old_snap);
	}
	if (new_snap != NULL) {
		screen_free(new_snap);
		free(new_snap);
	}
}

void
animation_dissolve_draw(struct client *c, struct animation *a, double t)
{
	struct grid_cell	 defaults;
	u_int			 row, col;
	u_int			 src_sx_old, src_sy_old;
	u_int			 src_sx_new, src_sy_new;
	u_int			 sx, sy;

	if (a->dissolve_old == NULL || a->dissolve_new == NULL)
		return;
	memcpy(&defaults, &grid_default_cell, sizeof defaults);

	src_sx_old = screen_size_x(a->dissolve_old);
	src_sy_old = screen_size_y(a->dissolve_old);
	src_sx_new = screen_size_x(a->dissolve_new);
	src_sy_new = screen_size_y(a->dissolve_new);
	sx = a->pane_w;
	if (sx > src_sx_old) sx = src_sx_old;
	if (sx > src_sx_new) sx = src_sx_new;
	sy = a->pane_h;
	if (sy > src_sy_old) sy = src_sy_old;
	if (sy > src_sy_new) sy = src_sy_new;

	for (row = 0; row < sy; row++) {
		u_int run_start = 0;
		int run_src = -1;
		int vy = a->pane_y0 + row;
		if (vy < 0 || vy >= (int)c->tty.sy)
			continue;
		for (col = 0; col < sx; col++) {
			int src = (dissolve_threshold(a, col, row) < t) ? 1 : 0;
			if (run_src == -1) {
				run_src = src;
				run_start = col;
				continue;
			}
			if (src != run_src) {
				struct screen *s = (run_src == 1) ?
				    a->dissolve_new : a->dissolve_old;
				tty_draw_line(&c->tty, s, run_start, row,
				    col - run_start,
				    a->pane_x0 + run_start, (u_int)vy,
				    &defaults, &a->dissolve_palette);
				run_src = src;
				run_start = col;
			}
		}
		if (run_src != -1) {
			struct screen *s = (run_src == 1) ?
			    a->dissolve_new : a->dissolve_old;
			tty_draw_line(&c->tty, s, run_start, row,
			    sx - run_start,
			    a->pane_x0 + run_start, (u_int)a->pane_y0 + row,
			    &defaults, &a->dissolve_palette);
		}
	}
}

void
animation_dissolve_free(struct animation *a)
{
	if (a->dissolve_old != NULL) {
		screen_free(a->dissolve_old);
		free(a->dissolve_old);
		a->dissolve_old = NULL;
	}
	if (a->dissolve_new != NULL) {
		screen_free(a->dissolve_new);
		free(a->dissolve_new);
		a->dissolve_new = NULL;
	}
}

static void
animation_dissolve_broadcast(struct window_pane *wp, struct screen *src_live,
    struct screen *src_target)
{
	struct client	*c;
	struct screen	*per_old, *per_new;

	if (wp == NULL || wp->window == NULL ||
	    src_live == NULL || src_target == NULL)
		return;

	TAILQ_FOREACH(c, &clients, entry) {
		if (c->session == NULL || c->session->curw == NULL)
			continue;
		if (c->session->curw->window != wp->window)
			continue;

		per_old = xcalloc(1, sizeof *per_old);
		animation_clone_screen(per_old, src_live);
		per_new = xcalloc(1, sizeof *per_new);
		animation_clone_screen(per_new, src_target);

		animation_begin_screen_dissolve(c, wp, per_old, per_new);
	}
}

void
animation_mode_enter(struct window_pane *wp, struct screen *old_s,
    struct screen *new_s)
{
	animation_dissolve_broadcast(wp, old_s, new_s);
}

void
animation_mode_exit(struct window_pane *wp, struct screen *old_s,
    struct screen *new_s)
{
	animation_dissolve_broadcast(wp, old_s, new_s);
}

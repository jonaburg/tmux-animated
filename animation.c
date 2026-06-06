/* $OpenBSD$ */

/*
 * Animation core: timer loop, easing, overlay callbacks, and dispatch to
 * per-kind draw/free functions implemented in animation-window.c,
 * animation-pane.c, and animation-scroll.c.
 */

#include <sys/types.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "tmux.h"
#include "animation-internal.h"

static void	animation_step_smoothdamp(struct animation *, double);
static void	animation_step_linear(struct animation *, uint64_t);
static void	animation_step_ease_in_out(struct animation *, uint64_t);
static int	animation_is_done(const struct animation *, uint64_t);

enum animation_easing
animation_easing_lookup(struct session *s)
{
	int	n = options_get_number(s->options, "animation-easing");

	switch (n) {
	case 0: return (ANIM_EASE_SMOOTHDAMP);
	case 1: return (ANIM_EASE_LINEAR);
	case 2: return (ANIM_EASE_IN_OUT);
	}
	return (ANIM_EASE_SMOOTHDAMP);
}

void
animation_cancel(struct client *c)
{
	if (c == NULL || c->animation == NULL)
		return;
	if (event_initialized(&c->animation_timer))
		evtimer_del(&c->animation_timer);
	server_client_clear_overlay(c);
}

int
animation_active(struct client *c)
{
	return (c != NULL && c->animation != NULL);
}

struct visible_ranges *
animation_check_cb(struct client *c, void *data, u_int px, u_int py, u_int nx)
{
	struct animation	*a = data;
	struct visible_ranges	*r = &a->vis;
	u_int			 row_l = px, row_r = px + nx;
	u_int			 cov_l, cov_r;

	(void)c;

	server_client_ensure_ranges(r, 2);
	r->used = 0;

	if (py < a->pane_y0 || py >= a->pane_y0 + a->pane_h) {
		r->ranges[r->used].px = px;
		r->ranges[r->used].nx = nx;
		r->used = 1;
		return (r);
	}

	cov_l = a->pane_x0;
	cov_r = a->pane_x0 + a->pane_w;

	if (row_l < cov_l) {
		u_int end = row_r < cov_l ? row_r : cov_l;
		r->ranges[r->used].px = row_l;
		r->ranges[r->used].nx = end - row_l;
		r->used++;
	}
	if (row_r > cov_r) {
		u_int start = row_l > cov_r ? row_l : cov_r;
		r->ranges[r->used].px = start;
		r->ranges[r->used].nx = row_r - start;
		r->used++;
	}
	return (r);
}

void
animation_draw_cb(struct client *c, void *data,
    __unused struct screen_redraw_ctx *rctx)
{
	struct animation	*a = data;
	overlay_check_cb	 saved_check;
	void			*saved_data;

	saved_check = c->overlay_check;
	saved_data = c->overlay_data;
	c->overlay_check = NULL;
	c->overlay_data = NULL;

	switch (a->kind) {
	case ANIM_PANE_LAYOUT: {
		double t;
		if (a->duration_ms == 0)
			t = 1;
		else {
			uint64_t now = get_timer();
			t = (double)(now - a->start_ms) /
			    (double)a->duration_ms;
		}
		if (t < 0) t = 0;
		if (t > 1) t = 1;
		animation_pane_draw(c, a, t);
		break;
	}
	case ANIM_SCROLL:
		animation_scroll_draw(c, a);
		break;
	case ANIM_SLIDE_WINDOW:
		animation_window_draw(c, a);
		break;
	}

	c->overlay_check = saved_check;
	c->overlay_data = saved_data;
}

int
animation_key_cb(__unused struct client *c, __unused void *data,
    __unused struct key_event *event)
{
	return (2);
}

void
animation_free_cb(struct client *c, void *data)
{
	struct animation	*a = data;

	if (c->animation == a)
		c->animation = NULL;

	switch (a->kind) {
	case ANIM_PANE_LAYOUT:
		animation_pane_free(a);
		break;
	case ANIM_SCROLL:
		animation_scroll_free(a);
		break;
	case ANIM_SLIDE_WINDOW:
		animation_window_free(a);
		break;
	}

	free(a->vis.ranges);
	free(a);
}

void
animation_resize_cb(struct client *c, __unused void *data)
{
	animation_cancel(c);
}

/* pos += (1 - exp(-dt/tau)) * (target - pos) */
static void
animation_step_smoothdamp(struct animation *a, double dt_ms)
{
	double	tau = (double)a->tau_ms;
	double	alpha;
	double	prev = a->pos;

	if (tau <= 0)
		tau = 1;
	alpha = 1.0 - exp(-dt_ms / tau);
	a->pos += alpha * (a->target_pos - a->pos);
	if (dt_ms > 0)
		a->vel = (a->pos - prev) / dt_ms;
}

static void
animation_step_linear(struct animation *a, uint64_t now)
{
	double	t;

	if (a->duration_ms == 0) {
		a->pos = a->target_pos;
		return;
	}
	t = (double)(now - a->start_ms) / (double)a->duration_ms;
	if (t < 0) t = 0;
	if (t > 1) t = 1;
	a->pos = a->start_pos + t * (a->target_pos - a->start_pos);
}

double
animation_ease_cubic_in_out(double t)
{
	if (t < 0.5)
		return (4.0 * t * t * t);
	t = 2.0 * t - 2.0;
	return (0.5 * t * t * t + 1.0);
}

static void
animation_step_ease_in_out(struct animation *a, uint64_t now)
{
	double	t, e;

	if (a->duration_ms == 0) {
		a->pos = a->target_pos;
		return;
	}
	t = (double)(now - a->start_ms) / (double)a->duration_ms;
	if (t < 0) t = 0;
	if (t > 1) t = 1;
	e = animation_ease_cubic_in_out(t);
	a->pos = a->start_pos + e * (a->target_pos - a->start_pos);
}

static int
animation_is_done(const struct animation *a, uint64_t now)
{
	return (now - a->start_ms >= a->duration_ms);
}

void
animation_frame_cb(__unused int fd, __unused short ev, void *arg)
{
	struct client		*c = arg;
	struct animation	*a;
	uint64_t		 now;
	double			 dt_ms;
	struct timeval		 tv;

	if ((a = c->animation) == NULL)
		return;

	now = get_timer();
	dt_ms = (double)(now - a->last_ms);
	if (dt_ms < 0) dt_ms = 0;
	a->last_ms = now;

	if (a->kind == ANIM_PANE_LAYOUT || a->kind == ANIM_SCROLL)
		goto check_done;

	switch (a->easing) {
	case ANIM_EASE_SMOOTHDAMP:
		animation_step_smoothdamp(a, dt_ms);
		break;
	case ANIM_EASE_LINEAR:
		animation_step_linear(a, now);
		break;
	case ANIM_EASE_IN_OUT:
		animation_step_ease_in_out(a, now);
		break;
	}

check_done:
	if (animation_is_done(a, now)) {
		a->pos = a->target_pos;
		animation_cancel(c);
		return;
	}

	server_redraw_client(c);
	tv.tv_sec = 0;
	tv.tv_usec = (long)a->frame_interval_ms * 1000L;
	evtimer_add(&c->animation_timer, &tv);
}

void
animation_clone_screen(struct screen *dst, struct screen *src)
{
	struct screen_write_ctx	ctx;
	u_int			 sx = screen_size_x(src);
	u_int			 sy = screen_size_y(src);

	screen_init(dst, sx, sy, 0);
	screen_write_start(&ctx, dst);
	screen_write_cursormove(&ctx, 0, 0, 0);
	/* Grids store scrollback first; visible rows live at hsize..hsize+sy-1. */
	screen_write_fast_copy(&ctx, src, 0, src->grid->hsize, sx, sy);
	screen_write_stop(&ctx);
}

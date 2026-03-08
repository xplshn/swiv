#ifndef SWIV_H
#define SWIV_H

#include <stdbool.h>

#include <wayland-client.h>
#include <wld/wayland.h>
#include <wld/wld.h>

#include "image.h"
#include "protocol/xdg-shell-client-protocol.h"

struct xkb_context;
struct xkb_keymap;
struct xkb_state;

enum swiv_action {
	SWIV_ACTION_QUIT = 0,
	SWIV_ACTION_TOGGLE_ANTIALIAS,
	SWIV_ACTION_TOGGLE_LOCK_WINDOW_ASPECT,
	SWIV_ACTION_ZOOM_IN,
	SWIV_ACTION_ZOOM_OUT,
};

struct swiv_wayland_state {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct xdg_wm_base *wm_base;
	struct wl_seat *seat;
	struct wl_keyboard *keyboard;
	struct wl_pointer *pointer;
	struct wl_surface *surface;
	struct xdg_surface *xdg_surface;
	struct xdg_toplevel *xdg_toplevel;
};

struct swiv_render_state {
	struct wld_context *wld_context;
	struct wld_surface *wld_surface;
	enum wld_format format;
	int surface_width;
	int surface_height;
};

struct swiv_view_state {
	struct image image;
	int pending_width;
	int pending_height;
	int window_width;
	int window_height;
	double pan_x;
	double pan_y;
	double zoom;
};

struct swiv_runtime_state {
	bool configured;
	bool running;
};

struct swiv_input_state {
	struct xkb_context *xkb_context;
	struct xkb_keymap *xkb_keymap;
	struct xkb_state *xkb_state;
	double pointer_x;
	double pointer_y;
	bool pointer_inside;
};

struct swiv_options {
	bool antialias;
	bool lock_window_aspect;
	float zoom_step;
};

struct swiv_ctx {
	struct swiv_wayland_state wl;
	struct swiv_render_state render;
	struct swiv_view_state view;
	struct swiv_runtime_state runtime;
	struct swiv_input_state input;
	struct swiv_options options;
};

extern struct swiv_ctx *swiv;

void handle_action(struct swiv_ctx *ctx, enum swiv_action action);
void render(struct swiv_ctx *ctx);
void zoom_at(struct swiv_ctx *ctx, double factor, double anchor_x, double anchor_y);
void aspect_fit(int in_w, int in_h, int img_w, int img_h, int *out_w, int *out_h);

#endif /* SWIV_H */


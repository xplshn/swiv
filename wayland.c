#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>

#include "swiv.h"
#include "wayland.h"

struct swiv_ctx *swiv = NULL;

enum {
	POINTER_BUTTON_LEFT = 0x110, /* BTN_LEFT (used by wl_pointer) */
};

enum {
	MOD_SHIFT = 1u << 0,
	MOD_CTRL = 1u << 1,
	MOD_ALT = 1u << 2,
	MOD_LOGO = 1u << 3,
};

struct keybind {
	uint32_t mods;
	xkb_keysym_t keysym;
	enum swiv_action action;
};

static void buffer_release(void *data, struct wl_buffer *wl);
static void keybind_perform(struct swiv_ctx *ctx, xkb_keysym_t keysym);
static void keyboard_clear_state(struct swiv_ctx *ctx);
static void keyboard_enter(void *data, struct wl_keyboard *keyboard,
                           uint32_t serial, struct wl_surface *surface,
                           struct wl_array *keys);
static void keyboard_key(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                         uint32_t time, uint32_t key, uint32_t state);
static void keyboard_keymap(void *data, struct wl_keyboard *keyboard,
                            uint32_t format, int32_t fd, uint32_t size);
static void keyboard_leave(void *data, struct wl_keyboard *keyboard,
                           uint32_t serial, struct wl_surface *surface);
static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard,
                               uint32_t serial, uint32_t mods_depressed,
                               uint32_t mods_latched, uint32_t mods_locked,
                               uint32_t group);
static void keyboard_repeat_info(void *data, struct wl_keyboard *keyboard,
                                 int32_t rate, int32_t delay);
static uint32_t mods_pressed(struct xkb_state *state);
static void pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time,
                         uint32_t axis, wl_fixed_t value);
static void pointer_axis_discrete(void *data, struct wl_pointer *pointer, uint32_t axis,
                                  int32_t discrete);
static void pointer_axis_source(void *data, struct wl_pointer *pointer, uint32_t source);
static void pointer_axis_stop(void *data, struct wl_pointer *pointer, uint32_t time,
                              uint32_t axis);
static void pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial,
                           uint32_t time, uint32_t button, uint32_t state);
static void pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial,
                          struct wl_surface *surface, wl_fixed_t surface_x,
                          wl_fixed_t surface_y);
static void pointer_frame(void *data, struct wl_pointer *pointer);
static void pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial,
                          struct wl_surface *surface);
static void pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time,
                           wl_fixed_t surface_x, wl_fixed_t surface_y);
static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface, uint32_t version);
static void registry_remove(void *data, struct wl_registry *registry, uint32_t name);
static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t capabilities);
static void seat_name(void *data, struct wl_seat *seat, const char *name);
static void xdg_surface_configure(void *data, struct xdg_surface *surface, uint32_t serial);
static void xdg_toplevel_close(void *data, struct xdg_toplevel *toplevel);
static void xdg_toplevel_configure(void *data, struct xdg_toplevel *toplevel,
                                   int32_t width, int32_t height,
                                   struct wl_array *states);
static void xdg_wm_base_ping(void *data, struct xdg_wm_base *base, uint32_t serial);

static const struct keybind keybinds[] = { /* TODO? move to config */
	{ .mods = 0, .keysym = XKB_KEY_a, .action = SWIV_ACTION_TOGGLE_ANTIALIAS },
	{ .mods = 0, .keysym = XKB_KEY_Escape, .action = SWIV_ACTION_QUIT },
	{ .mods = 0, .keysym = XKB_KEY_q, .action = SWIV_ACTION_QUIT },
	{ .mods = 0, .keysym = XKB_KEY_l, .action = SWIV_ACTION_TOGGLE_LOCK_WINDOW_ASPECT },
};

static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = keyboard_keymap,
	.enter = keyboard_enter,
	.leave = keyboard_leave,
	.key = keyboard_key,
	.modifiers = keyboard_modifiers,
	.repeat_info = keyboard_repeat_info,
};

static const struct wl_pointer_listener pointer_listener = {
	.enter = pointer_enter,
	.leave = pointer_leave,
	.motion = pointer_motion,
	.button = pointer_button,
	.axis = pointer_axis,
	.frame = pointer_frame,
	.axis_source = pointer_axis_source,
	.axis_stop = pointer_axis_stop,
	.axis_discrete = pointer_axis_discrete,
};

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

static void buffer_release(void *data, struct wl_buffer *wl)
{
	struct wld_buffer *buffer = data;
	(void)wl;

	if (swiv && swiv->render.wld_surface)
		wld_surface_release(swiv->render.wld_surface, buffer);
}

static void keybind_perform(struct swiv_ctx *ctx, xkb_keysym_t keysym)
{
	/* find matching keybind and execute action */
	uint32_t mods = mods_pressed(ctx->input.xkb_state);
	for (size_t i = 0; i < sizeof keybinds / sizeof keybinds[0]; ++i) {
		if (keybinds[i].keysym == keysym && keybinds[i].mods == mods) {
			handle_action(ctx, keybinds[i].action);
			return;
		}
	}
}

static void keyboard_clear_state(struct swiv_ctx *ctx)
{
	/* deref and clear xkb state and keymap */
	if (ctx->input.xkb_state) {
		xkb_state_unref(ctx->input.xkb_state);
		ctx->input.xkb_state = NULL;
	}
	if (ctx->input.xkb_keymap) {
		xkb_keymap_unref(ctx->input.xkb_keymap);
		ctx->input.xkb_keymap = NULL;
	}
}

static void keyboard_enter(void *data, struct wl_keyboard *keyboard,
                           uint32_t serial, struct wl_surface *surface,
                           struct wl_array *keys)
{
	(void)data;
	(void)keyboard;
	(void)serial;
	(void)surface;
	(void)keys;
}

static void keyboard_key(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                         uint32_t time, uint32_t key, uint32_t state)
{
	(void)keyboard;
	(void)serial;
	(void)time;

	struct swiv_ctx *ctx = data;
	xkb_keysym_t keysym;
	
	if (state != WL_KEYBOARD_KEY_STATE_PRESSED || !ctx->input.xkb_state)
		return;

	/* get keysym for key event */
	keysym = xkb_state_key_get_one_sym(ctx->input.xkb_state, key + 8);
	if (keysym != XKB_KEY_NoSymbol)
		keybind_perform(ctx, keysym);
}

static void keyboard_keymap(void *data, struct wl_keyboard *keyboard,
                            uint32_t format, int32_t fd, uint32_t size)
{
	struct swiv_ctx *ctx = data;
	char *mapped;
	struct xkb_keymap *keymap;
	struct xkb_state *state;
	(void)keyboard;

	/* only support xkb v1 keymaps */
	if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 || fd < 0 || size == 0) {
		if (fd >= 0)
			close(fd);
		return;
	}

	mapped = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	if (mapped == MAP_FAILED) {
		close(fd);
		return;
	}

	/* clear old state and keymap */
	keymap = xkb_keymap_new_from_string(ctx->input.xkb_context, mapped,
	                                    XKB_KEYMAP_FORMAT_TEXT_V1,
	                                    XKB_KEYMAP_COMPILE_NO_FLAGS);
	munmap(mapped, size);
	close(fd);

	if (!keymap)
		return;

	/* create new xkb state from keymap */
	state = xkb_state_new(keymap);
	if (!state) {
		xkb_keymap_unref(keymap);
		return;
	}

	keyboard_clear_state(ctx);
	ctx->input.xkb_keymap = keymap;
	ctx->input.xkb_state = state;
}

static void keyboard_leave(void *data, struct wl_keyboard *keyboard,
                           uint32_t serial, struct wl_surface *surface)
{
	(void)data;
	(void)keyboard;
	(void)serial;
	(void)surface;
}

static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard,
                               uint32_t serial, uint32_t mods_depressed,
                               uint32_t mods_latched, uint32_t mods_locked,
                               uint32_t group)
{
	(void)keyboard;
	(void)serial;

	struct swiv_ctx *ctx = data;
	if (!ctx->input.xkb_state)
		return;

	/* update xkb state with new modifiers */
	xkb_state_update_mask(ctx->input.xkb_state,
	                      mods_depressed,
	                      mods_latched,
	                      mods_locked,
	                      0,
	                      0,
	                      group);
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *keyboard,
                                 int32_t rate, int32_t delay)
{
	(void)data;
	(void)keyboard;
	(void)rate;
	(void)delay;
}

static uint32_t mods_pressed(struct xkb_state *state)
{
	uint32_t mods = 0;

	if (!state)
		return mods;

	if (xkb_state_mod_name_is_active(state, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE))
		mods |= MOD_SHIFT;
	if (xkb_state_mod_name_is_active(state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE))
		mods |= MOD_CTRL;
	if (xkb_state_mod_name_is_active(state, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE))
		mods |= MOD_ALT;
	if (xkb_state_mod_name_is_active(state, XKB_MOD_NAME_LOGO, XKB_STATE_MODS_EFFECTIVE))
		mods |= MOD_LOGO;

	return mods;
}

static void pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time,
                         uint32_t axis, wl_fixed_t value)
{
	struct swiv_ctx *ctx = data;
	double scroll;
	double anchor_x;
	double anchor_y;
	uint32_t mods;
	(void)pointer;
	(void)time;

	if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL)
		return;

	mods = mods_pressed(ctx->input.xkb_state);
	if ((mods & MOD_SHIFT) || !ctx->input.pointer_inside) {
		if (mods & MOD_SHIFT) {
			ctx->view.pan_x = 0.0;
			ctx->view.pan_y = 0.0;
		}
		anchor_x = ctx->view.window_width * 0.5;
		anchor_y = ctx->view.window_height * 0.5;
	} else {
		anchor_x = ctx->input.pointer_x;
		anchor_y = ctx->input.pointer_y;
	}

	scroll = wl_fixed_to_double(value);
	if (scroll <= -1.0) /* scroll up */
		zoom_at(ctx, ctx->options.zoom_step, anchor_x, anchor_y);
	else if (scroll >= 1.0) /* scroll down */
		zoom_at(ctx, 1.0 / ctx->options.zoom_step, anchor_x, anchor_y);
}

static void pointer_axis_discrete(void *data, struct wl_pointer *pointer, uint32_t axis,
                                  int32_t discrete)
{
	(void)data;
	(void)pointer;
	(void)axis;
	(void)discrete;
}

static void pointer_axis_source(void *data, struct wl_pointer *pointer, uint32_t source)
{
	(void)data;
	(void)pointer;
	(void)source;
}

static void pointer_axis_stop(void *data, struct wl_pointer *pointer, uint32_t time,
                              uint32_t axis)
{
	(void)data;
	(void)pointer;
	(void)time;
	(void)axis;
}

static void pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial,
                           uint32_t time, uint32_t button, uint32_t state)
{
	struct swiv_ctx *ctx = data;
	(void)pointer;
	(void)serial;
	(void)time;

	if (button != POINTER_BUTTON_LEFT)
		return;

	if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
		ctx->input.pointer_dragging = true;
	} else if (state == WL_POINTER_BUTTON_STATE_RELEASED) {
		ctx->input.pointer_dragging = false;
	}
}

static void pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial,
                          struct wl_surface *surface, wl_fixed_t surface_x,
                          wl_fixed_t surface_y)
{
	struct swiv_ctx *ctx = data;
	(void)pointer;
	(void)serial;
	(void)surface;

	ctx->input.pointer_inside = true;
	ctx->input.pointer_x = wl_fixed_to_double(surface_x);
	ctx->input.pointer_y = wl_fixed_to_double(surface_y);
}

static void pointer_frame(void *data, struct wl_pointer *pointer)
{
	(void)data;
	(void)pointer;
}

static void pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial,
                          struct wl_surface *surface)
{
	struct swiv_ctx *ctx = data;
	(void)pointer;
	(void)serial;
	(void)surface;

	ctx->input.pointer_inside = false;
	ctx->input.pointer_dragging = false;
}

static void pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time,
                           wl_fixed_t surface_x, wl_fixed_t surface_y)
{
	struct swiv_ctx *ctx = data;
	double x;
	double y;
	double dx;
	double dy;
	(void)pointer;
	(void)time;

	x = wl_fixed_to_double(surface_x);
	y = wl_fixed_to_double(surface_y);
	dx = x - ctx->input.pointer_x;
	dy = y - ctx->input.pointer_y;
	ctx->input.pointer_x = x;
	ctx->input.pointer_y = y;

	if (ctx->input.pointer_dragging) {
		ctx->view.pan_x += dx;
		ctx->view.pan_y += dy;
		if (ctx->runtime.configured)
			render(ctx);
	}
}

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface, uint32_t version)
{
	struct swiv_ctx *ctx = data;

	/* bind wl_compositor, xdg_wm_base, wl_seat */
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		uint32_t bind_version = version < 4 ? version : 4;
		ctx->wl.compositor = wl_registry_bind(registry, name,
		                                      &wl_compositor_interface,
		                                      bind_version);
	} else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
		ctx->wl.wm_base = wl_registry_bind(registry, name,
		                                   &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(ctx->wl.wm_base, &wm_base_listener, ctx);
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		uint32_t bind_version = version < 5 ? version : 5;
		ctx->wl.seat = wl_registry_bind(registry, name, &wl_seat_interface, bind_version);
		wl_seat_add_listener(ctx->wl.seat, &seat_listener, ctx);
	}
}

static void registry_remove(void *data, struct wl_registry *registry, uint32_t name)
{
	(void)data;
	(void)registry;
	(void)name;
}

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t capabilities)
{
	struct swiv_ctx *ctx = data;

	/* if seat has pointer cap, get wl_pointer, add listener */
	if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !ctx->wl.pointer) {
		ctx->wl.pointer = wl_seat_get_pointer(seat);
		if (ctx->wl.pointer)
			wl_pointer_add_listener(ctx->wl.pointer, &pointer_listener, ctx);
	} else if (!(capabilities & WL_SEAT_CAPABILITY_POINTER) && ctx->wl.pointer) {
		wl_pointer_destroy(ctx->wl.pointer);
		ctx->wl.pointer = NULL;
	}

	/* if seat has keyboard cap, get wl_keyboard, add listener */
	if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && !ctx->wl.keyboard) {
		ctx->wl.keyboard = wl_seat_get_keyboard(seat);
		if (ctx->wl.keyboard)
			wl_keyboard_add_listener(ctx->wl.keyboard, &keyboard_listener, ctx);
	} else if (!(capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && ctx->wl.keyboard) {
		wl_keyboard_destroy(ctx->wl.keyboard);
		ctx->wl.keyboard = NULL;
		keyboard_clear_state(ctx);
	}
}

static void seat_name(void *data, struct wl_seat *seat, const char *name)
{
	(void)data;
	(void)seat;
	(void)name;
}

static void xdg_surface_configure(void *data, struct xdg_surface *surface, uint32_t serial)
{
	struct swiv_ctx *ctx = data;

	/* ack configure */
	xdg_surface_ack_configure(surface, serial);
	if (!ctx->runtime.configured) {
		ctx->view.pending_width = ctx->view.image.width;
		ctx->view.pending_height = ctx->view.image.height;
	}
	ctx->runtime.configured = true;
	render(ctx);
}

static void xdg_toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
	struct swiv_ctx *ctx = data;
	(void)toplevel;
	ctx->runtime.running = false;
}

static void xdg_toplevel_configure(void *data, struct xdg_toplevel *toplevel,
                                   int32_t width, int32_t height,
                                   struct wl_array *states)
{
	struct swiv_ctx *ctx = data;
	(void)toplevel;
	(void)states;

	if (ctx->options.lock_window_aspect) {
		int fitted_w = 0;
		int fitted_h = 0;
		if (width > 0 || height > 0) {
			aspect_fit(width, height, ctx->view.image.width, ctx->view.image.height,
			           &fitted_w, &fitted_h);
			ctx->view.pending_width = fitted_w;
			ctx->view.pending_height = fitted_h;
		}
	} else {
		if (width > 0)
			ctx->view.pending_width = width;
		if (height > 0)
			ctx->view.pending_height = height;
	}
}

static void xdg_wm_base_ping(void *data, struct xdg_wm_base *base, uint32_t serial)
{
	(void)data;
	xdg_wm_base_pong(base, serial);
}

const struct wl_buffer_listener buffer_listener = { .release = buffer_release, };
const struct xdg_wm_base_listener wm_base_listener = { .ping = xdg_wm_base_ping, };
const struct xdg_surface_listener xdg_surface_listener = { .configure = xdg_surface_configure, };
const struct xdg_toplevel_listener xdg_toplevel_listener = {
	.configure = xdg_toplevel_configure,
	.close = xdg_toplevel_close,
};
const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_remove,
};


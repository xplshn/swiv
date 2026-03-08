#include "image.h"

#include <pixman-1/pixman.h>
#include <wayland-client.h>

#include <wld/wld.h>
#include <wld/wayland.h>

#include "protocol/xdg-shell-client-protocol.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct app {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct xdg_wm_base *wm_base;
	struct wl_surface *surface;
	struct xdg_surface *xdg_surface;
	struct xdg_toplevel *xdg_toplevel;

	struct wld_context *wld_context;
	struct wld_surface *wld_surface;
	enum wld_format format;

	struct image image;
	bool configured;
	bool running;

	int pending_width;
	int pending_height;
	int window_width;
	int window_height;
	int surface_width;
	int surface_height;
};

static struct app *g_app;

static void buffer_release(void *data, struct wl_buffer *wl)
{
	struct wld_buffer *buffer = data;
	(void)wl;

	if (g_app && g_app->wld_surface)
		wld_surface_release(g_app->wld_surface, buffer);
}

static const struct wl_buffer_listener buffer_listener = {
	.release = buffer_release,
};

static void app_update_size(struct app *app)
{
	int width = app->window_width;
	int height = app->window_height;

	if (app->pending_width > 0)
		width = app->pending_width;
	else if (width == 0)
		width = app->image.width;

	if (app->pending_height > 0)
		height = app->pending_height;
	else if (height == 0)
		height = app->image.height;

	app->window_width = width;
	app->window_height = height;
	app->pending_width = 0;
	app->pending_height = 0;
}

static void aspect_fit(int in_w, int in_h, int img_w, int img_h, int *out_w, int *out_h)
{
	if (in_w <= 0 || in_h <= 0) {
		*out_w = img_w;
		*out_h = img_h;
		return;
	}

	/* aspect ratio, fit size */
	double scale_w = (double)in_w / (double)img_w;
	double scale_h = (double)in_h / (double)img_h;
	double scale = scale_w < scale_h ? scale_w : scale_h;

	int w = (int)(img_w * scale + 0.5);
	int h = (int)(img_h * scale + 0.5);

	if (w < 1)
		w = 1;
	if (h < 1)
		h = 1;

	*out_w = w;
	*out_h = h;
}

static void app_render(struct app *app)
{
	uint32_t flags = WLD_FLAG_MAP;

	/* create wld context if not there */
	if (!app->wld_context) {
		app->wld_context = wld_wayland_create_context(app->display, WLD_SHM, WLD_NONE);
		if (!app->wld_context) {
		fprintf(stderr, "swiv: failed to create wld wl context\n");
			app->running = false;
			return;
		}

		app->format = WLD_FORMAT_ARGB8888;
		if (!wld_wayland_has_format(app->wld_context, app->format)) {
			app->format = WLD_FORMAT_XRGB8888;
			if (!wld_wayland_has_format(app->wld_context, app->format)) {
				fprintf(stderr, "swiv: no supported pixel format\n");
				app->running = false;
				return;
			}
		}

		if (app->format == WLD_FORMAT_XRGB8888 && app->image.has_alpha)
			image_force_opaque(&app->image);
	}

	app_update_size(app);

	/* TODO? geom */
	if (app->xdg_surface)
		xdg_surface_set_window_geometry(app->xdg_surface, 0, 0,
		                                app->window_width, app->window_height);

	/* if size changed, create new wld surface */
	if (app->surface_width != app->window_width || app->surface_height != app->window_height) {
		/* destroy old surface */
		if (app->wld_surface) {
			wl_display_roundtrip(app->display);
			wld_destroy_surface(app->wld_surface);
		}

		/* create new surface */
		app->wld_surface = wld_wayland_create_surface(
			app->wld_context,
			(uint32_t)app->window_width,
			(uint32_t)app->window_height,
			app->format,
			flags,
			app->surface);
		if (!app->wld_surface) {
			fprintf(stderr, "swiv: failed to create wld surface\n");
			app->running = false;
			return;
		}

		app->surface_width = app->window_width;
		app->surface_height = app->window_height;
	}

	/* get wl_buffer */
	struct wld_buffer *buffer = wld_surface_take(app->wld_surface);
	if (!buffer) {
		fprintf(stderr, "swiv: failed to get surface buffer\n");
		app->running = false;
		return;
	}

	/* map, clear buffer */
	if (!wld_map(buffer)) {
		fprintf(stderr, "swiv: failed to map surface buffer\n");
		app->running = false;
		return;
	}
	memset(buffer->map, 0, (size_t)buffer->pitch * buffer->height);

	/* aspect ratio, fit size, offsets */
	double scale_x = (double)app->window_width / (double)app->image.width;
	double scale_y = (double)app->window_height / (double)app->image.height;
	double scale = scale_x < scale_y ? scale_x : scale_y;

	int draw_w = (int)(app->image.width * scale + 0.5);
	int draw_h = (int)(app->image.height * scale + 0.5);
	if (draw_w < 1)
		draw_w = 1;
	if (draw_h < 1)
		draw_h = 1;

	int offset_x = (app->window_width - draw_w) / 2;
	int offset_y = (app->window_height - draw_h) / 2;

	/* create pixman images */
	pixman_format_code_t dst_format = (app->format == WLD_FORMAT_ARGB8888)
		? PIXMAN_a8r8g8b8 : PIXMAN_x8r8g8b8;
	pixman_format_code_t src_format = app->image.has_alpha
		? PIXMAN_a8r8g8b8 : PIXMAN_x8r8g8b8;

	pixman_image_t *dst = pixman_image_create_bits(
		dst_format,
		app->window_width,
		app->window_height,
		buffer->map,
		buffer->pitch);

	pixman_image_t *src = pixman_image_create_bits(
		src_format,
		app->image.width,
		app->image.height,
		(uint32_t *)app->image.pixels,
		app->image.width * 4);

	/* transform, composite */
	if (dst && src) {
		pixman_transform_t transform;
		pixman_transform_init_scale(&transform,
		                            pixman_double_to_fixed((double)app->image.width / (double)draw_w),
		                            pixman_double_to_fixed((double)app->image.height / (double)draw_h));
		pixman_image_set_transform(src, &transform);
		pixman_image_set_filter(src, PIXMAN_FILTER_BILINEAR, NULL, 0);

		pixman_image_composite32(PIXMAN_OP_SRC, src, NULL, dst,
		                         0, 0, 0, 0,
		                         offset_x, offset_y, draw_w, draw_h);
	}

	if (src)
		pixman_image_unref(src);
	if (dst)
		pixman_image_unref(dst);

	wld_unmap(buffer);

	/* export wl_buffer */
	union wld_object object;
	if (!wld_export(buffer, WLD_WAYLAND_OBJECT_BUFFER, &object)) {
		fprintf(stderr, "swiv: failed to export wl_buffer\n");
		app->running = false;
		return;
	}

	/* add buffer listener for release events */
	struct wl_buffer *wl_buffer = object.ptr;
	if (!wl_proxy_get_listener((struct wl_proxy *)wl_buffer))
		wl_buffer_add_listener(wl_buffer, &buffer_listener, buffer);

	/* commit */
	wl_surface_attach(app->surface, wl_buffer, 0, 0);
	wl_surface_damage(app->surface, 0, 0, app->window_width, app->window_height);
	wl_surface_commit(app->surface);
	wl_display_flush(app->display);
}

static void xdg_wm_base_ping(void *data, struct xdg_wm_base *base, uint32_t serial)
{
	(void)data;
	xdg_wm_base_pong(base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
	.ping = xdg_wm_base_ping,
};

static void xdg_surface_configure(void *data, struct xdg_surface *surface, uint32_t serial)
{
	struct app *app = data;

	/* ack configure */
	xdg_surface_ack_configure(surface, serial);
	if (!app->configured) {
		app->pending_width = app->image.width;
		app->pending_height = app->image.height;
	}
	app->configured = true;
	app_render(app);
}

static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_configure,
};

static void xdg_toplevel_configure(void *data, struct xdg_toplevel *toplevel,
                                  int32_t width, int32_t height,
                                  struct wl_array *states)
{
	struct app *app = data;
	(void)toplevel;
	(void)states;

	if (width > 0 || height > 0) {
		int fitted_w = 0;
		int fitted_h = 0;
		aspect_fit(width, height, app->image.width, app->image.height,
		           &fitted_w, &fitted_h);
		app->pending_width = fitted_w;
		app->pending_height = fitted_h;
	}
}

static void xdg_toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
	struct app *app = data;
	(void)toplevel;
	app->running = false;
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
	.configure = xdg_toplevel_configure,
	.close = xdg_toplevel_close,
};

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface, uint32_t version)
{
	struct app *app = data;

	/* bind wl_compositor and xdg_wm_base */
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		uint32_t bind_version = version < 4 ? version : 4;
		app->compositor = wl_registry_bind(registry, name,
		                                   &wl_compositor_interface,
		                                   bind_version);
	} else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
		app->wm_base = wl_registry_bind(registry, name,
		                               &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(app->wm_base, &wm_base_listener, app);
	}
}

static void registry_remove(void *data, struct wl_registry *registry, uint32_t name)
{
	(void)data;
	(void)registry;
	(void)name;
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_remove,
};

static void app_cleanup(struct app *app)
{
	if (app->wld_surface)
		wld_destroy_surface(app->wld_surface);
	if (app->wld_context)
		wld_destroy_context(app->wld_context);

	if (app->xdg_toplevel)
		xdg_toplevel_destroy(app->xdg_toplevel);
	if (app->xdg_surface)
		xdg_surface_destroy(app->xdg_surface);
	if (app->surface)
		wl_surface_destroy(app->surface);
	if (app->wm_base)
		xdg_wm_base_destroy(app->wm_base);
	if (app->compositor)
		wl_compositor_destroy(app->compositor);
	if (app->registry)
		wl_registry_destroy(app->registry);
	if (app->display)
		wl_display_disconnect(app->display);

	free_image(&app->image);
}

int main(int argc, char **argv)
{
	struct app app = {0};
	char err[256];

	g_app = &app;

	if (argc != 2) {
		fprintf(stderr, "usage: %s [/path/to/image]\n", argv[0]);
		return EXIT_FAILURE;
	}

	if (load_image(argv[1], &app.image, err, sizeof err) != IMAGE_OK) {
		fprintf(stderr, "swiv: failed to load image: %s\n", err);
		return EXIT_FAILURE;
	}

	app.display = wl_display_connect(NULL);
	if (!app.display) {
		fprintf(stderr, "swiv: failed to connect to wl display\n");
		free_image(&app.image);
		return EXIT_FAILURE;
	}

	/* registry */
	app.registry = wl_display_get_registry(app.display);
	wl_registry_add_listener(app.registry, &registry_listener, &app);
	wl_display_roundtrip(app.display);

	if (!app.compositor || !app.wm_base) {
		fprintf(stderr, "swiv: compositor or xdg_wm_base not available\n");
		app_cleanup(&app);
		return EXIT_FAILURE;
	}

	app.surface = wl_compositor_create_surface(app.compositor);
	if (!app.surface) {
		fprintf(stderr, "swiv: failed to create wl_surface\n");
		app_cleanup(&app);
		return EXIT_FAILURE;
	}

	/* XDG surface */
	app.xdg_surface = xdg_wm_base_get_xdg_surface(app.wm_base, app.surface);
	xdg_surface_add_listener(app.xdg_surface, &xdg_surface_listener, &app);

	/* XDG toplevel */
	app.xdg_toplevel = xdg_surface_get_toplevel(app.xdg_surface);
	xdg_toplevel_add_listener(app.xdg_toplevel, &xdg_toplevel_listener, &app);
	xdg_toplevel_set_title(app.xdg_toplevel, "swiv");

	/* set initial window geom to image size */
	xdg_surface_set_window_geometry(app.xdg_surface, 0, 0,
	                                app.image.width, app.image.height);

	wl_surface_commit(app.surface);

	app.running = true;
	while (app.running && wl_display_dispatch(app.display) != -1)
		;

	app_cleanup(&app);
	return EXIT_SUCCESS;
}

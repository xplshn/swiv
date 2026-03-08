#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pixman-1/pixman.h>
#include <xkbcommon/xkbcommon.h>

#include "swiv.h"
#include "wayland.h"

static void app_cleanup(struct swiv_ctx *ctx);
static void app_update_size(struct swiv_ctx *ctx);
static void run(struct swiv_ctx *ctx);
static bool setup(struct swiv_ctx *ctx);

static void app_cleanup(struct swiv_ctx *ctx)
{
	if (ctx->render.wld_surface)
		wld_destroy_surface(ctx->render.wld_surface);
	if (ctx->render.wld_context)
		wld_destroy_context(ctx->render.wld_context);

	if (ctx->wl.xdg_toplevel)
		xdg_toplevel_destroy(ctx->wl.xdg_toplevel);
	if (ctx->wl.xdg_surface)
		xdg_surface_destroy(ctx->wl.xdg_surface);
	if (ctx->wl.keyboard)
		wl_keyboard_destroy(ctx->wl.keyboard);
	if (ctx->wl.pointer)
		wl_pointer_destroy(ctx->wl.pointer);
	if (ctx->wl.seat)
		wl_seat_destroy(ctx->wl.seat);
	if (ctx->wl.surface)
		wl_surface_destroy(ctx->wl.surface);
	if (ctx->wl.wm_base)
		xdg_wm_base_destroy(ctx->wl.wm_base);
	if (ctx->wl.compositor)
		wl_compositor_destroy(ctx->wl.compositor);
	if (ctx->wl.registry)
		wl_registry_destroy(ctx->wl.registry);
	if (ctx->wl.display)
		wl_display_disconnect(ctx->wl.display);

	if (ctx->input.xkb_state)
		xkb_state_unref(ctx->input.xkb_state);
	if (ctx->input.xkb_keymap)
		xkb_keymap_unref(ctx->input.xkb_keymap);
	if (ctx->input.xkb_context)
		xkb_context_unref(ctx->input.xkb_context);

	image_free(&ctx->view.image);
}

static void app_update_size(struct swiv_ctx *ctx)
{
	int width = ctx->view.window_width;
	int height = ctx->view.window_height;

	if (ctx->view.pending_width > 0)
		width = ctx->view.pending_width;
	else if (width == 0)
		width = ctx->view.image.width;

	if (ctx->view.pending_height > 0)
		height = ctx->view.pending_height;
	else if (height == 0)
		height = ctx->view.image.height;

	ctx->view.window_width = width;
	ctx->view.window_height = height;
	ctx->view.pending_width = 0;
	ctx->view.pending_height = 0;
}

void aspect_fit(int in_w, int in_h, int img_w, int img_h, int *out_w, int *out_h)
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

void handle_action(struct swiv_ctx *ctx, enum swiv_action action)
{
	switch (action) {
	case SWIV_ACTION_QUIT:
		ctx->runtime.running = false;
		break;
	case SWIV_ACTION_TOGGLE_ANTIALIAS:
		ctx->options.antialias = !ctx->options.antialias;
		if (ctx->runtime.configured)
			render(ctx);
		break;
	case SWIV_ACTION_TOGGLE_LOCK_WINDOW_ASPECT:
		ctx->options.lock_window_aspect = !ctx->options.lock_window_aspect;
		if (ctx->runtime.configured)
			render(ctx);
		break;
	case SWIV_ACTION_ZOOM_IN:
		ctx->view.zoom *= ctx->options.zoom_step;
		if (ctx->runtime.configured)
			render(ctx);
		break;
	case SWIV_ACTION_ZOOM_OUT:
		ctx->view.zoom /= ctx->options.zoom_step;
		if (ctx->runtime.configured)
			render(ctx);
		break;
	}
}

void zoom_at(struct swiv_ctx *ctx, double factor, double anchor_x, double anchor_y)
{
	double fit_scale;
	double old_scale;
	double new_scale;
	double old_draw_w;
	double old_draw_h;
	double new_draw_w;
	double new_draw_h;
	double old_center_x;
	double old_center_y;
	double new_center_x;
	double new_center_y;
	double old_offset_x;
	double old_offset_y;
	double image_x;
	double image_y;
	double new_offset_x;
	double new_offset_y;

	if (!ctx->runtime.configured || factor <= 0.0)
		return;

	app_update_size(ctx);

	fit_scale = (double)ctx->view.window_width / (double)ctx->view.image.width;
	if ((double)ctx->view.window_height / (double)ctx->view.image.height < fit_scale)
		fit_scale = (double)ctx->view.window_height / (double)ctx->view.image.height;

	/* find img coords of anchor point before zoom */
	old_scale = fit_scale * ctx->view.zoom;
	old_draw_w = (double)ctx->view.image.width * old_scale;
	old_draw_h = (double)ctx->view.image.height * old_scale;
	old_center_x = ((double)ctx->view.window_width - old_draw_w) * 0.5;
	old_center_y = ((double)ctx->view.window_height - old_draw_h) * 0.5;
	old_offset_x = old_center_x + ctx->view.pan_x;
	old_offset_y = old_center_y + ctx->view.pan_y;
	image_x = (anchor_x - old_offset_x) / old_scale;
	image_y = (anchor_y - old_offset_y) / old_scale;

	/* apply zoom */
	ctx->view.zoom *= factor;

	/* find new pan to keep anchor point stable */
	new_scale = fit_scale * ctx->view.zoom;
	new_draw_w = (double)ctx->view.image.width * new_scale;
	new_draw_h = (double)ctx->view.image.height * new_scale;
	new_center_x = ((double)ctx->view.window_width - new_draw_w) * 0.5;
	new_center_y = ((double)ctx->view.window_height - new_draw_h) * 0.5;
	new_offset_x = anchor_x - image_x * new_scale;
	new_offset_y = anchor_y - image_y * new_scale;
	ctx->view.pan_x = new_offset_x - new_center_x;
	ctx->view.pan_y = new_offset_y - new_center_y;

	render(ctx);
}

void render(struct swiv_ctx *ctx)
{
	uint32_t flags = WLD_FLAG_MAP;

	app_update_size(ctx);

	/* TODO? geom */
	if (ctx->wl.xdg_surface)
		xdg_surface_set_window_geometry(ctx->wl.xdg_surface, 0, 0,
		                                ctx->view.window_width, ctx->view.window_height);

	/* if size changed, create new wld surface */
	if (ctx->render.surface_width != ctx->view.window_width ||
	    ctx->render.surface_height != ctx->view.window_height) {
		/* destroy old surface */
		if (ctx->render.wld_surface) {
			wl_display_roundtrip(ctx->wl.display);
			wld_destroy_surface(ctx->render.wld_surface);
		}

		/* create new surface */
		ctx->render.wld_surface = wld_wayland_create_surface(
			ctx->render.wld_context,
			(uint32_t)ctx->view.window_width,
			(uint32_t)ctx->view.window_height,
			ctx->render.format,
			flags,
			ctx->wl.surface);
		if (!ctx->render.wld_surface) {
			fprintf(stderr, "swiv: failed to create wld surface\n");
			ctx->runtime.running = false;
			return;
		}

		ctx->render.surface_width = ctx->view.window_width;
		ctx->render.surface_height = ctx->view.window_height;
	}

	/* get wl_buffer */
	struct wld_buffer *buffer = wld_surface_take(ctx->render.wld_surface);
	if (!buffer) {
		fprintf(stderr, "swiv: failed to get surface buffer\n");
		ctx->runtime.running = false;
		return;
	}

	/* map, clear buffer */
	if (!wld_map(buffer)) {
		fprintf(stderr, "swiv: failed to map surface buffer\n");
		ctx->runtime.running = false;
		return;
	}
	memset(buffer->map, 0, (size_t)buffer->pitch * buffer->height);

	/* aspect ratio, fit size, offsets */
	double scale_x = (double)ctx->view.window_width / (double)ctx->view.image.width;
	double scale_y = (double)ctx->view.window_height / (double)ctx->view.image.height;
	double scale = (scale_x < scale_y ? scale_x : scale_y) * ctx->view.zoom;

	int draw_w = (int)(ctx->view.image.width * scale + 0.5);
	int draw_h = (int)(ctx->view.image.height * scale + 0.5);
	if (draw_w < 1)
		draw_w = 1;
	if (draw_h < 1)
		draw_h = 1;

	int offset_x = (ctx->view.window_width - draw_w) / 2 + (int)ctx->view.pan_x;
	int offset_y = (ctx->view.window_height - draw_h) / 2 + (int)ctx->view.pan_y;

	/* create pixman images */
	pixman_format_code_t dst_format = (ctx->render.format == WLD_FORMAT_ARGB8888)
		? PIXMAN_a8r8g8b8 : PIXMAN_x8r8g8b8;
	pixman_format_code_t src_format = ctx->view.image.has_alpha
		? PIXMAN_a8r8g8b8 : PIXMAN_x8r8g8b8;

	pixman_image_t *dst = pixman_image_create_bits(
		dst_format,
		ctx->view.window_width,
		ctx->view.window_height,
		buffer->map,
		buffer->pitch);

	pixman_image_t *src = pixman_image_create_bits(
		src_format,
		ctx->view.image.width,
		ctx->view.image.height,
		(uint32_t *)ctx->view.image.pixels,
		ctx->view.image.width * 4);

	/* transform, composite */
	if (dst && src) {
		pixman_transform_t transform;
		pixman_transform_init_scale(&transform,
		                            pixman_double_to_fixed((double)ctx->view.image.width / (double)draw_w),
		                            pixman_double_to_fixed((double)ctx->view.image.height / (double)draw_h));
		pixman_image_set_transform(src, &transform);
		pixman_filter_t filter = ctx->options.antialias
			? PIXMAN_FILTER_BILINEAR
			: PIXMAN_FILTER_NEAREST;
		pixman_image_set_filter(src, filter, NULL, 0);

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
		ctx->runtime.running = false;
		return;
	}

	/* add buffer listener for release events */
	struct wl_buffer *wl_buffer = object.ptr;
	if (!wl_proxy_get_listener((struct wl_proxy *)wl_buffer))
		wl_buffer_add_listener(wl_buffer, &buffer_listener, buffer);

	/* commit */
	wl_surface_attach(ctx->wl.surface, wl_buffer, 0, 0);
	wl_surface_damage(ctx->wl.surface, 0, 0, ctx->view.window_width, ctx->view.window_height);
	wl_surface_commit(ctx->wl.surface);
	wl_display_flush(ctx->wl.display);
}

static bool setup(struct swiv_ctx *ctx)
{
	/* display */
	ctx->wl.display = wl_display_connect(NULL);
	if (!ctx->wl.display) {
		fprintf(stderr, "swiv: failed to connect to wl display\n");
		return false;
	}

	/* xkb */
	ctx->input.xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (!ctx->input.xkb_context) {
		fprintf(stderr, "swiv: failed to create xkb context\n");
		return false;
	}

	/* registry */
	ctx->wl.registry = wl_display_get_registry(ctx->wl.display);
	wl_registry_add_listener(ctx->wl.registry, &registry_listener, ctx);
	wl_display_roundtrip(ctx->wl.display);

	/* surface */
	if (!ctx->wl.compositor || !ctx->wl.wm_base) {
		fprintf(stderr, "swiv: compositor or xdg_wm_base not available\n");
		return false;
	}

	ctx->wl.surface = wl_compositor_create_surface(ctx->wl.compositor);
	if (!ctx->wl.surface) {
		fprintf(stderr, "swiv: failed to create wl_surface\n");
		return false;
	}

	/* xdg surface */
	ctx->wl.xdg_surface = xdg_wm_base_get_xdg_surface(ctx->wl.wm_base, ctx->wl.surface);
	xdg_surface_add_listener(ctx->wl.xdg_surface, &xdg_surface_listener, ctx);

	/* xdg toplevel */
	ctx->wl.xdg_toplevel = xdg_surface_get_toplevel(ctx->wl.xdg_surface);
	xdg_toplevel_add_listener(ctx->wl.xdg_toplevel, &xdg_toplevel_listener, ctx);
	xdg_toplevel_set_title(ctx->wl.xdg_toplevel, "swiv");

	/* set initial window geom to image size */
	xdg_surface_set_window_geometry(ctx->wl.xdg_surface, 0, 0,
	                                ctx->view.image.width, ctx->view.image.height);

	wl_surface_commit(ctx->wl.surface);

	/* wld context */
	ctx->render.wld_context = wld_wayland_create_context(ctx->wl.display, WLD_SHM, WLD_NONE);
	if (!ctx->render.wld_context) {
		fprintf(stderr, "swiv: failed to create wld context\n");
		return false;
	}

	ctx->render.format = WLD_FORMAT_ARGB8888;
	if (!wld_wayland_has_format(ctx->render.wld_context, ctx->render.format)) {
		ctx->render.format = WLD_FORMAT_XRGB8888;
		if (!wld_wayland_has_format(ctx->render.wld_context, ctx->render.format)) {
			fprintf(stderr, "swiv: no supported pixel format\n");
			return false;
		}
	}

	if (ctx->render.format == WLD_FORMAT_XRGB8888 && ctx->view.image.has_alpha)
		image_force_opaque(&ctx->view.image);

	/* TODO options */
	ctx->options.antialias = true;
	ctx->options.lock_window_aspect = true;
	ctx->options.zoom_step = 1.1;
	ctx->view.zoom = 1;
	ctx->view.pan_x = 0;
	ctx->view.pan_y = 0;

	return true;
}

static void run(struct swiv_ctx *ctx)
{
	ctx->runtime.running = true;
	while (ctx->runtime.running && wl_display_dispatch(ctx->wl.display) != -1)
		;
}

int main(int argc, char **argv)
{
	struct swiv_ctx ctx = {0};
	char err[256];
	swiv = &ctx;

	if (argc != 2) {
		fprintf(stderr, "usage: %s [/path/to/image]\n", argv[0]);
		return EXIT_FAILURE;
	}

	if (image_load(argv[1], &ctx.view.image, err, sizeof err) != IMAGE_OK) {
		fprintf(stderr, "swiv: failed to load image: %s\n", err);
		return EXIT_FAILURE;
	}

	if (!setup(&ctx)) {
		app_cleanup(&ctx);
		return EXIT_FAILURE;
	}

	run(&ctx);

	app_cleanup(&ctx);
	return EXIT_SUCCESS;
}


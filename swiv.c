#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pixman-1/pixman.h>

#include "swiv.h"
#include "wayland.h"

static void app_cleanup(struct swiv_ctx *ctx);
static void app_update_size(struct swiv_ctx *ctx);
static void run(struct swiv_ctx *ctx);
static bool setup(struct swiv_ctx *ctx);

static void app_cleanup(struct swiv_ctx *ctx)
{
	if (ctx->wld_surface)
		wld_destroy_surface(ctx->wld_surface);
	if (ctx->wld_context)
		wld_destroy_context(ctx->wld_context);

	if (ctx->xdg_toplevel)
		xdg_toplevel_destroy(ctx->xdg_toplevel);
	if (ctx->xdg_surface)
		xdg_surface_destroy(ctx->xdg_surface);
	if (ctx->surface)
		wl_surface_destroy(ctx->surface);
	if (ctx->wm_base)
		xdg_wm_base_destroy(ctx->wm_base);
	if (ctx->compositor)
		wl_compositor_destroy(ctx->compositor);
	if (ctx->registry)
		wl_registry_destroy(ctx->registry);
	if (ctx->display)
		wl_display_disconnect(ctx->display);

	image_free(&ctx->image);
}

static void app_update_size(struct swiv_ctx *ctx)
{
	int width = ctx->window_width;
	int height = ctx->window_height;

	if (ctx->pending_width > 0)
		width = ctx->pending_width;
	else if (width == 0)
		width = ctx->image.width;

	if (ctx->pending_height > 0)
		height = ctx->pending_height;
	else if (height == 0)
		height = ctx->image.height;

	ctx->window_width = width;
	ctx->window_height = height;
	ctx->pending_width = 0;
	ctx->pending_height = 0;
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

void app_render(struct swiv_ctx *ctx)
{
	uint32_t flags = WLD_FLAG_MAP;

	app_update_size(ctx);

	/* TODO? geom */
	if (ctx->xdg_surface)
		xdg_surface_set_window_geometry(ctx->xdg_surface, 0, 0,
		                                ctx->window_width, ctx->window_height);

	/* if size changed, create new wld surface */
	if (ctx->surface_width != ctx->window_width || ctx->surface_height != ctx->window_height) {
		/* destroy old surface */
		if (ctx->wld_surface) {
			wl_display_roundtrip(ctx->display);
			wld_destroy_surface(ctx->wld_surface);
		}

		/* create new surface */
		ctx->wld_surface = wld_wayland_create_surface(
			ctx->wld_context,
			(uint32_t)ctx->window_width,
			(uint32_t)ctx->window_height,
			ctx->format,
			flags,
			ctx->surface);
		if (!ctx->wld_surface) {
			fprintf(stderr, "swiv: failed to create wld surface\n");
			ctx->running = false;
			return;
		}

		ctx->surface_width = ctx->window_width;
		ctx->surface_height = ctx->window_height;
	}

	/* get wl_buffer */
	struct wld_buffer *buffer = wld_surface_take(ctx->wld_surface);
	if (!buffer) {
		fprintf(stderr, "swiv: failed to get surface buffer\n");
		ctx->running = false;
		return;
	}

	/* map, clear buffer */
	if (!wld_map(buffer)) {
		fprintf(stderr, "swiv: failed to map surface buffer\n");
		ctx->running = false;
		return;
	}
	memset(buffer->map, 0, (size_t)buffer->pitch * buffer->height);

	/* aspect ratio, fit size, offsets */
	double scale_x = (double)ctx->window_width / (double)ctx->image.width;
	double scale_y = (double)ctx->window_height / (double)ctx->image.height;
	double scale = scale_x < scale_y ? scale_x : scale_y;

	int draw_w = (int)(ctx->image.width * scale + 0.5);
	int draw_h = (int)(ctx->image.height * scale + 0.5);
	if (draw_w < 1)
		draw_w = 1;
	if (draw_h < 1)
		draw_h = 1;

	int offset_x = (ctx->window_width - draw_w) / 2;
	int offset_y = (ctx->window_height - draw_h) / 2;

	/* create pixman images */
	pixman_format_code_t dst_format = (ctx->format == WLD_FORMAT_ARGB8888)
		? PIXMAN_a8r8g8b8 : PIXMAN_x8r8g8b8;
	pixman_format_code_t src_format = ctx->image.has_alpha
		? PIXMAN_a8r8g8b8 : PIXMAN_x8r8g8b8;

	pixman_image_t *dst = pixman_image_create_bits(
		dst_format,
		ctx->window_width,
		ctx->window_height,
		buffer->map,
		buffer->pitch);

	pixman_image_t *src = pixman_image_create_bits(
		src_format,
		ctx->image.width,
		ctx->image.height,
		(uint32_t *)ctx->image.pixels,
		ctx->image.width * 4);

	/* transform, composite */
	if (dst && src) {
		pixman_transform_t transform;
		pixman_transform_init_scale(&transform,
		                            pixman_double_to_fixed((double)ctx->image.width / (double)draw_w),
		                            pixman_double_to_fixed((double)ctx->image.height / (double)draw_h));
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
		ctx->running = false;
		return;
	}

	/* add buffer listener for release events */
	struct wl_buffer *wl_buffer = object.ptr;
	if (!wl_proxy_get_listener((struct wl_proxy *)wl_buffer))
		wl_buffer_add_listener(wl_buffer, &buffer_listener, buffer);

	/* commit */
	wl_surface_attach(ctx->surface, wl_buffer, 0, 0);
	wl_surface_damage(ctx->surface, 0, 0, ctx->window_width, ctx->window_height);
	wl_surface_commit(ctx->surface);
	wl_display_flush(ctx->display);
}

static bool setup(struct swiv_ctx *ctx)
{
	/* display */
	ctx->display = wl_display_connect(NULL);
	if (!ctx->display) {
		fprintf(stderr, "swiv: failed to connect to wl display\n");
		return false;
	}

	/* registry */
	ctx->registry = wl_display_get_registry(ctx->display);
	wl_registry_add_listener(ctx->registry, &registry_listener, ctx);
	wl_display_roundtrip(ctx->display);

	if (!ctx->compositor || !ctx->wm_base) {
		fprintf(stderr, "swiv: compositor or xdg_wm_base not available\n");
		return false;
	}

	ctx->surface = wl_compositor_create_surface(ctx->compositor);
	if (!ctx->surface) {
		fprintf(stderr, "swiv: failed to create wl_surface\n");
		return false;
	}

	/* xdg surface */
	ctx->xdg_surface = xdg_wm_base_get_xdg_surface(ctx->wm_base, ctx->surface);
	xdg_surface_add_listener(ctx->xdg_surface, &xdg_surface_listener, ctx);

	/* xdg toplevel */
	ctx->xdg_toplevel = xdg_surface_get_toplevel(ctx->xdg_surface);
	xdg_toplevel_add_listener(ctx->xdg_toplevel, &xdg_toplevel_listener, ctx);
	xdg_toplevel_set_title(ctx->xdg_toplevel, "swiv");

	/* set initial window geom to image size */
	xdg_surface_set_window_geometry(ctx->xdg_surface, 0, 0,
	                                ctx->image.width, ctx->image.height);

	wl_surface_commit(ctx->surface);

	/* wld context */
	ctx->wld_context = wld_wayland_create_context(ctx->display, WLD_SHM, WLD_NONE);
	if (!ctx->wld_context) {
		fprintf(stderr, "swiv: failed to create wld context\n");
		return false;
	}

	ctx->format = WLD_FORMAT_ARGB8888;
	if (!wld_wayland_has_format(ctx->wld_context, ctx->format)) {
		ctx->format = WLD_FORMAT_XRGB8888;
		if (!wld_wayland_has_format(ctx->wld_context, ctx->format)) {
			fprintf(stderr, "swiv: no supported pixel format\n");
			return false;
		}
	}

	if (ctx->format == WLD_FORMAT_XRGB8888 && ctx->image.has_alpha)
		image_force_opaque(&ctx->image);

	return true;
}

static void run(struct swiv_ctx *ctx)
{
	ctx->running = true;
	while (ctx->running && wl_display_dispatch(ctx->display) != -1)
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

	if (image_load(argv[1], &ctx.image, err, sizeof err) != IMAGE_OK) {
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


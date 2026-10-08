#define _GNU_SOURCE
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"

struct screen {
	struct wl_output *output;
	struct wl_surface *surface;
	struct zwlr_layer_surface_v1 *layer;
	char name[64];
	int width, height;
	int ready;
	struct wl_buffer *buffer;
	void *pixels;
	size_t buf_size;
	struct wl_list link;
};

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct zwlr_layer_shell_v1 *shell;
static struct wl_list screens;

static unsigned char *image;
static int img_w, img_h;
static const char *target_output;

static void die(const char *msg)
{
	fprintf(stderr, "wl-paper: %s\n", msg);
	exit(1);
}

static const char APP_NAMESPACE[] = "wl-paper";

static void blit_nn(uint32_t *dst, int dst_w, int dst_h,
		     const unsigned char *src, int src_w, int src_h)
{
	for (int y = 0; y < dst_h; y++) {
		int src_y = y * src_h / dst_h;
		const unsigned char *row = src + (size_t)src_y * src_w * 4;
		for (int x = 0; x < dst_w; x++) {
			int src_x = x * src_w / dst_w;
			const unsigned char *p = row + (size_t)src_x * 4;
			dst[x] = (uint32_t)p[3] << 24 | (uint32_t)p[0] << 16 |
				 (uint32_t)p[1] << 8 | (uint32_t)p[2];
		}
		dst += dst_w;
	}
}

static struct wl_buffer *create_shm_buffer(struct wl_shm *shm_obj, int w,
					    int h, size_t *size_out,
					    void **map_out)
{
	int stride = w * 4;
	size_t size = (size_t)stride * h;

	int fd = memfd_create(APP_NAMESPACE, 0);
	if (fd < 0 || ftruncate(fd, (off_t)size) < 0) {
		if (fd >= 0)
			close(fd);
		return NULL;
	}

	void *map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED) {
		close(fd);
		return NULL;
	}

	struct wl_shm_pool *pool =
		wl_shm_create_pool(shm_obj, fd, (int32_t)size);
	if (!pool) {
		munmap(map, size);
		close(fd);
		return NULL;
	}
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(
		pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	if (!buffer) {
		munmap(map, size);
		return NULL;
	}

	*size_out = size;
	*map_out = map;
	return buffer;
}

static void destroy_buffer(struct screen *s)
{
	if (!s->buffer)
		return;
	wl_buffer_destroy(s->buffer);
	munmap(s->pixels, s->buf_size);
	s->buffer = NULL;
	s->pixels = NULL;
	s->buf_size = 0;
}

static void destroy_screen(struct screen *s)
{
	destroy_buffer(s);
	if (s->layer)
		zwlr_layer_surface_v1_destroy(s->layer);
	if (s->surface)
		wl_surface_destroy(s->surface);
	if (s->output)
		wl_output_release(s->output);
	wl_list_remove(&s->link);
	free(s);
}

static void paint(struct screen *s)
{
	size_t size = 0;
	void *map = NULL;

	struct wl_buffer *buffer = create_shm_buffer(shm, s->width, s->height,
						     &size, &map);
	if (!buffer)
		die("could not create shared memory");

	blit_nn(map, s->width, s->height, image, img_w, img_h);

	destroy_buffer(s);
	s->buffer = buffer;
	s->pixels = map;
	s->buf_size = size;

	wl_surface_attach(s->surface, buffer, 0, 0);
	wl_surface_damage_buffer(s->surface, 0, 0, INT32_MAX, INT32_MAX);
	wl_surface_commit(s->surface);
}

static void screen_configure(struct screen *s, uint32_t serial, uint32_t width,
			     uint32_t height)
{
	zwlr_layer_surface_v1_ack_configure(s->layer, serial);
	if (width == 0 || height == 0)
		return;
	s->width = (int)width;
	s->height = (int)height;
	s->ready = 1;
	paint(s);
}

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *layer,
			    uint32_t serial, uint32_t width, uint32_t height)
{
	(void)layer;
	screen_configure(data, serial, width, height);
}

static void layer_closed(void *data, struct zwlr_layer_surface_v1 *layer)
{
	(void)layer;
	destroy_screen(data);
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
	.configure = layer_configure,
	.closed = layer_closed,
};

static void output_geometry(void *data, struct wl_output *output, int32_t x,
			    int32_t y, int32_t pw, int32_t ph, int32_t sub,
			    const char *make, const char *model, int32_t tr)
{
	(void)data;
	(void)output;
	(void)x;
	(void)y;
	(void)pw;
	(void)ph;
	(void)sub;
	(void)make;
	(void)model;
	(void)tr;
}

static void output_mode(void *data, struct wl_output *output, uint32_t flags,
			int32_t w, int32_t h, int32_t refresh)
{
	(void)data;
	(void)output;
	(void)flags;
	(void)w;
	(void)h;
	(void)refresh;
}

static void output_done(void *data, struct wl_output *output)
{
	(void)data;
	(void)output;
}

static void output_scale(void *data, struct wl_output *output, int32_t factor)
{
	(void)data;
	(void)output;
	(void)factor;
}

static void output_name(void *data, struct wl_output *output, const char *name)
{
	(void)output;
	struct screen *s = data;
	snprintf(s->name, sizeof(s->name), "%s", name);
}

static void output_description(void *data, struct wl_output *output,
			       const char *desc)
{
	(void)data;
	(void)output;
	(void)desc;
}

static const struct wl_output_listener output_listener = {
	.geometry = output_geometry,
	.mode = output_mode,
	.done = output_done,
	.scale = output_scale,
	.name = output_name,
	.description = output_description,
};

static int screen_matches(const struct screen *s, const char *wanted)
{
	return !wanted || strcmp(s->name, wanted) == 0;
}

static int screen_setup(struct screen *s, struct wl_compositor *comp,
			 struct zwlr_layer_shell_v1 *sh)
{
	s->surface = wl_compositor_create_surface(comp);
	if (!s->surface)
		return -1;
	s->layer = zwlr_layer_shell_v1_get_layer_surface(
		sh, s->surface, s->output,
		ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, APP_NAMESPACE);
	if (!s->layer) {
		wl_surface_destroy(s->surface);
		s->surface = NULL;
		return -1;
	}
	zwlr_layer_surface_v1_set_anchor(
		s->layer, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
				  ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
				  ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
				  ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
	zwlr_layer_surface_v1_set_exclusive_zone(s->layer, -1);
	zwlr_layer_surface_v1_add_listener(s->layer, &layer_listener, s);
	wl_surface_commit(s->surface);
	s->ready = 0;
	return 0;
}

static void add_screen(struct wl_registry *reg, uint32_t id, uint32_t version)
{
	struct screen *s = calloc(1, sizeof(*s));
	if (!s)
		die("out of memory");

	uint32_t bind_ver = version < 4 ? version : 4;
	s->output = wl_registry_bind(reg, id, &wl_output_interface, bind_ver);
	if (version >= 4)
		wl_output_add_listener(s->output, &output_listener, s);
	else
		snprintf(s->name, sizeof(s->name), "%u", id);

	wl_list_insert(screens.prev, &s->link);
}

static void registry_global(void *data, struct wl_registry *reg, uint32_t id,
			    const char *iface, uint32_t version)
{
	(void)data;
	if (strcmp(iface, wl_compositor_interface.name) == 0) {
		compositor = wl_registry_bind(reg, id, &wl_compositor_interface, 4);
	} else if (strcmp(iface, wl_shm_interface.name) == 0) {
		shm = wl_registry_bind(reg, id, &wl_shm_interface, 1);
	} else if (strcmp(iface, zwlr_layer_shell_v1_interface.name) == 0) {
		shell = wl_registry_bind(reg, id, &zwlr_layer_shell_v1_interface, 1);
	} else if (strcmp(iface, wl_output_interface.name) == 0) {
		add_screen(reg, id, version);
	}
}

static void registry_remove(void *data, struct wl_registry *reg, uint32_t id)
{
	(void)data;
	(void)reg;
	(void)id;
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_remove,
};

int main(int argc, char *argv[])
{
	const char *path = NULL;

	wl_list_init(&screens);

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
			target_output = argv[++i];
		} else if (strcmp(argv[i], "-h") == 0 ||
			   strcmp(argv[i], "--help") == 0) {
			printf("usage: wl-paper [-o output] <image>\n");
			return 0;
		} else if (argv[i][0] == '-') {
			fprintf(stderr, "wl-paper: unknown option %s\n", argv[i]);
			fprintf(stderr, "usage: wl-paper [-o output] <image>\n");
			return 1;
		} else {
			path = argv[i];
		}
	}

	if (!path) {
		fprintf(stderr, "usage: wl-paper [-o output] <image>\n");
		return 1;
	}

	image = stbi_load(path, &img_w, &img_h, NULL, 4);
	if (!image) {
		fprintf(stderr, "wl-paper: could not load %s\n", path);
		return 1;
	}

	display = wl_display_connect(NULL);
	if (!display)
		die("could not connect to wayland compositor");

	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	/* The second roundtrip waits for output names. */
	wl_display_roundtrip(display);

	if (!compositor || !shm || !shell)
		die("compositor does not support layer shell");

	struct screen *s, *tmp;
	wl_list_for_each_safe(s, tmp, &screens, link) {
		if (!screen_matches(s, target_output)) {
			destroy_screen(s);
			continue;
		}
		if (screen_setup(s, compositor, shell) < 0)
			die("could not create layer surface");
	}

	if (target_output && wl_list_empty(&screens)) {
		fprintf(stderr, "wl-paper: no output named %s\n", target_output);
		return 1;
	}
	if (wl_list_empty(&screens))
		die("no outputs found");

	for (;;) {
		int waiting = 0;
		wl_list_for_each(s, &screens, link) {
			if (s->ready == 0)
				waiting = 1;
		}
		if (!waiting)
			break;
		if (wl_display_roundtrip(display) < 0)
			die("lost connection to compositor");
		if (wl_list_empty(&screens))
			return 0;
	}

	while (wl_display_dispatch(display) >= 0) {
		if (wl_list_empty(&screens))
			break;
	}

	return 0;
}

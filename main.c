#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

struct target_config {
    const char *monitor_name;
    const char *image_path;
};

struct monitor_output {
    struct app_state *state;
    struct wl_output *wl_output;
    uint32_t registry_name;
    char *name;
    int32_t width;
    int32_t height;
    int32_t scale;
    bool done_received;

    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer_surface;
    struct wl_buffer *buffer;
    void *shm_data;
    size_t shm_size;

    const char *assigned_image;
    struct monitor_output *next;
};

struct app_state {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct zwlr_layer_shell_v1 *layer_shell;

    struct target_config *targets;
    int target_count;

    struct monitor_output *outputs;
    bool running;
};

// Shared Memory and image configuration
static int create_shm_file(off_t size) {
    char name[] = "/wl-paper-XXXXXX";
    int fd = -1;
    for (int tries = 0; tries < 100; ++tries) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        long r = ts.tv_nsec ^ (long)getpid();
        for (int i = 0; i < 6; ++i) {
            name[10 + i] = 'A' + (r & 15) + ((r & 16) ? 32 : 0);
            r >>= 5;
        }
        fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) {
            shm_unlink(name);
            break;
        }
        if (errno != EEXIST) {
            return -1;
        }
    }
    if (fd < 0) return -1;
    if (ftruncate(fd, size) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static struct wl_buffer *create_image_buffer(struct monitor_output *out,
                                             const char *image_path,
                                             uint32_t width, uint32_t height) {
    int img_w, img_h, channels;
    unsigned char *pixels = stbi_load(image_path, &img_w, &img_h, &channels, 4);
    if (!pixels) {
        fprintf(stderr, "Error: Failed to load image '%s': %s\n",
                image_path, stbi_failure_reason());
        return NULL;
    }

    uint32_t stride = width * 4;
    size_t size = (size_t)stride * height;

    int fd = create_shm_file(size);
    if (fd < 0) {
        stbi_image_free(pixels);
        return NULL;
    }

    void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        stbi_image_free(pixels);
        return NULL;
    }

    struct wl_shm_pool *pool = wl_shm_create_pool(out->state->shm, fd, size);
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(
        pool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);

    /* Center-crop cover scaling and RGBA -> ARGB8888 conversion */
    uint32_t *dst = (uint32_t *)data;
    float scale_x = (float)img_w / (float)width;
    float scale_y = (float)img_h / (float)height;
    float scale = (scale_x < scale_y) ? scale_x : scale_y;
    int crop_w = (int)(width * scale);
    int crop_h = (int)(height * scale);
    int offset_x = (img_w - crop_w) / 2;
    int offset_y = (img_h - crop_h) / 2;

    for (uint32_t y = 0; y < height; ++y) {
        int src_y = offset_y + (int)(y * scale);
        if (src_y >= img_h) src_y = img_h - 1;
        if (src_y < 0) src_y = 0;

        for (uint32_t x = 0; x < width; ++x) {
            int src_x = offset_x + (int)(x * scale);
            if (src_x >= img_w) src_x = img_w - 1;
            if (src_x < 0) src_x = 0;

            unsigned char *p = pixels + ((size_t)src_y * img_w + src_x) * 4;
            dst[y * width + x] = ((uint32_t)p[3] << 24) |
                                 ((uint32_t)p[0] << 16) |
                                 ((uint32_t)p[1] << 8)  |
                                 ((uint32_t)p[2]);
        }
    }

    stbi_image_free(pixels);

    if (out->shm_data) {
        munmap(out->shm_data, out->shm_size);
    }
    out->shm_data = data;
    out->shm_size = size;
    return buffer;
}

/* --- Layer Surface Callbacks --- */

static void layer_surface_configure(void *data,
                                    struct zwlr_layer_surface_v1 *layer_surface,
                                    uint32_t serial, uint32_t width, uint32_t height) {
    struct monitor_output *out = data;
    zwlr_layer_surface_v1_ack_configure(layer_surface, serial);

    if (width == 0) width = out->width > 0 ? (uint32_t)out->width : 1920;
    if (height == 0) height = out->height > 0 ? (uint32_t)out->height : 1080;

    if (out->buffer) {
        wl_buffer_destroy(out->buffer);
        out->buffer = NULL;
    }

    out->buffer = create_image_buffer(out, out->assigned_image, width, height);
    if (out->buffer) {
        wl_surface_attach(out->surface, out->buffer, 0, 0);
        wl_surface_damage_buffer(out->surface, 0, 0, INT32_MAX, INT32_MAX);
        wl_surface_commit(out->surface);
    }
}

static void layer_surface_closed(void *data,
                                 struct zwlr_layer_surface_v1 *layer_surface) {
    (void)layer_surface;
    struct monitor_output *out = data;
    if (out->layer_surface) {
        zwlr_layer_surface_v1_destroy(out->layer_surface);
        out->layer_surface = NULL;
    }
    if (out->surface) {
        wl_surface_destroy(out->surface);
        out->surface = NULL;
    }
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_configure,
    .closed = layer_surface_closed,
};

/* --- Per-Monitor Matching & Surface Initialization --- */

static const char *match_target_image(struct app_state *state, const char *output_name) {
    const char *default_image = NULL;
    for (int i = 0; i < state->target_count; ++i) {
        if (state->targets[i].monitor_name == NULL) {
            default_image = state->targets[i].image_path;
        } else if (output_name && strcmp(state->targets[i].monitor_name, output_name) == 0) {
            return state->targets[i].image_path;
        }
    }
    return default_image;
}

static void setup_output_wallpaper(struct monitor_output *out) {
    struct app_state *state = out->state;
    if (out->layer_surface || !state->compositor || !state->shm || !state->layer_shell) {
        return;
    }

    const char *img = match_target_image(state, out->name);
    if (!img) {
        return; /* Skip monitors not specified by the user */
    }

    out->assigned_image = img;
    out->surface = wl_compositor_create_surface(state->compositor);
    out->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        state->layer_shell, out->surface, out->wl_output,
        ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, "wallpaper");

    zwlr_layer_surface_v1_set_size(out->layer_surface, 0, 0);
    zwlr_layer_surface_v1_set_anchor(
        out->layer_surface,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
    zwlr_layer_surface_v1_set_exclusive_zone(out->layer_surface, -1);
    zwlr_layer_surface_v1_add_listener(out->layer_surface, &layer_surface_listener, out);
    wl_surface_commit(out->surface);
}

/* --- wl_output Listener --- */

static void output_geometry(void *data, struct wl_output *wl_output, int32_t x, int32_t y,
                            int32_t physical_width, int32_t physical_height,
                            int32_t subpixel, const char *make, const char *model,
                            int32_t transform) {
    (void)data; (void)wl_output; (void)x; (void)y;
    (void)physical_width; (void)physical_height;
    (void)subpixel; (void)make; (void)model; (void)transform;
}

static void output_mode(void *data, struct wl_output *wl_output, uint32_t flags,
                        int32_t width, int32_t height, int32_t refresh) {
    (void)wl_output; (void)refresh;
    struct monitor_output *out = data;
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        out->width = width;
        out->height = height;
    }
}

static void output_done(void *data, struct wl_output *wl_output) {
    (void)wl_output;
    struct monitor_output *out = data;
    out->done_received = true;
    setup_output_wallpaper(out);
}

static void output_scale(void *data, struct wl_output *wl_output, int32_t factor) {
    (void)wl_output;
    struct monitor_output *out = data;
    out->scale = factor;
}

static void output_name(void *data, struct wl_output *wl_output, const char *name) {
    (void)wl_output;
    struct monitor_output *out = data;
    free(out->name);
    out->name = strdup(name);
}

static void output_description(void *data, struct wl_output *wl_output,
                               const char *description) {
    (void)data; (void)wl_output; (void)description;
}

static const struct wl_output_listener output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
    .name = output_name,
    .description = output_description,
};

/* --- Registry Listener --- */

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
                            const char *interface, uint32_t version) {
    struct app_state *state = data;

    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        state->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        state->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        state->layer_shell = wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, 1);
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        uint32_t bind_ver = version < 4 ? version : 4;
        struct monitor_output *out = calloc(1, sizeof(*out));
        out->state = state;
        out->scale = 1;
        out->registry_name = name;
        out->wl_output = wl_registry_bind(registry, name, &wl_output_interface, bind_ver);
        wl_output_add_listener(out->wl_output, &output_listener, out);

        out->next = state->outputs;
        state->outputs = out;
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
    (void)registry;
    struct app_state *state = data;
    struct monitor_output **curr = &state->outputs;
    while (*curr) {
        if ((*curr)->registry_name == name) {
            struct monitor_output *removed = *curr;
            *curr = removed->next;
            if (removed->layer_surface) zwlr_layer_surface_v1_destroy(removed->layer_surface);
            if (removed->surface) wl_surface_destroy(removed->surface);
            if (removed->buffer) wl_buffer_destroy(removed->buffer);
            if (removed->shm_data) munmap(removed->shm_data, removed->shm_size);
            if (removed->wl_output) wl_output_destroy(removed->wl_output);
            free(removed->name);
            free(removed);
            break;
        }
        curr = &((*curr)->next);
    }
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

/* --- CLI Argument Parsing --- */

static void print_usage(const char *prog) {
    fprintf(stderr,
            "Usage:\n"
            "  %s <image_path>                          Set wallpaper on all monitors (default)\n"
            "  %s <image_path> <monitor_name>           Set wallpaper on a specific monitor\n"
            "  %s <monitor_name> <image_path>           Set wallpaper on a specific monitor\n"
            "  %s -m <monitor_name> <image_path> [...]  Set wallpaper per monitor (repeatable)\n",
            prog, prog, prog, prog);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    struct app_state state = {0};
    state.targets = calloc((size_t)argc, sizeof(struct target_config));
    state.running = true;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            free(state.targets);
            return 0;
        } else if (strcmp(argv[i], "-m") == 0 || strcmp(argv[i], "--monitor") == 0 ||
                   strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) {
            if (i + 2 >= argc) {
                fprintf(stderr, "Error: %s requires <monitor_name> <image_path>\n", argv[i]);
                print_usage(argv[0]);
                free(state.targets);
                return 1;
            }
            state.targets[state.target_count].monitor_name = argv[i + 1];
            state.targets[state.target_count].image_path = argv[i + 2];
            state.target_count++;
            i += 2;
        } else {
            /* Positional arguments: support both <image> [monitor] and <monitor> <image> */
            const char *arg1 = argv[i];
            const char *arg2 = (i + 1 < argc && argv[i + 1][0] != '-') ? argv[++i] : NULL;

            if (!arg2) {
                state.targets[state.target_count].monitor_name = NULL;
                state.targets[state.target_count].image_path = arg1;
            } else if (access(arg1, F_OK) != 0 && access(arg2, F_OK) == 0) {
                /* First arg is not a file, second arg is an existing file -> <monitor> <image> */
                state.targets[state.target_count].monitor_name = arg1;
                state.targets[state.target_count].image_path = arg2;
            } else {
                /* Default positional order -> <image> <monitor> */
                state.targets[state.target_count].monitor_name = arg2;
                state.targets[state.target_count].image_path = arg1;
            }
            state.target_count++;
        }
    }

    state.display = wl_display_connect(NULL);
    if (!state.display) {
        fprintf(stderr, "Error: Failed to connect to Wayland display\n");
        free(state.targets);
        return 1;
    }

    state.registry = wl_display_get_registry(state.display);
    wl_registry_add_listener(state.registry, &registry_listener, &state);

    /* First roundtrip binds globals; second roundtrip receives wl_output name/mode events */
    wl_display_roundtrip(state.display);
    wl_display_roundtrip(state.display);

    if (!state.compositor || !state.shm || !state.layer_shell) {
        fprintf(stderr, "Error: Compositor does not support wlr-layer-shell\n");
        wl_display_disconnect(state.display);
        free(state.targets);
        return 1;
    }

    /* Ensure all matched outputs are initialized even if wl_output arrived before layer_shell */
    bool any_matched = false;
    for (struct monitor_output *out = state.outputs; out; out = out->next) {
        setup_output_wallpaper(out);
        if (out->layer_surface) {
            any_matched = true;
        }
    }

    if (!any_matched) {
        fprintf(stderr, "Error: No connected monitor matched your argument.\n");
        fprintf(stderr, "Connected monitors:\n");
        for (struct monitor_output *out = state.outputs; out; out = out->next) {
            fprintf(stderr, "  * %s (%dx%d)\n",
                    out->name ? out->name : "unknown", out->width, out->height);
        }
        wl_display_disconnect(state.display);
        free(state.targets);
        return 1;
    }

    while (state.running && wl_display_dispatch(state.display) != -1) {
        /* Keep client alive to maintain wallpaper surfaces */
    }

    wl_display_disconnect(state.display);
    free(state.targets);
    return 0;
}

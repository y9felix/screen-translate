#define _GNU_SOURCE

#include <cairo/cairo.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

struct buffer {
	struct wl_buffer *wl_buf;
	void *data;
	size_t size;
	int width;
	int height;
};

static void die(const char *msg);
static int create_shm_buffer(int width, int height, struct buffer *b);
static void destroy_shm_buffer(struct buffer *b);
static void render(void);
static void load_images(void);
static void sig_handler(int sig);
static void layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *surface,
    uint32_t serial, uint32_t width, uint32_t height);
static void layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *surface);
static void keyboard_keymap(void *data, struct wl_keyboard *kb, uint32_t format,
    int32_t fd, uint32_t size);
static void keyboard_enter(void *data, struct wl_keyboard *kb, uint32_t serial,
    struct wl_surface *surf, struct wl_array *keys);
static void keyboard_leave(void *data, struct wl_keyboard *kb, uint32_t serial,
    struct wl_surface *surf);
static void keyboard_key(void *data, struct wl_keyboard *kb, uint32_t serial,
    uint32_t time, uint32_t key, uint32_t state);
static void keyboard_modifiers(void *data, struct wl_keyboard *kb, uint32_t serial,
    uint32_t mods_depressed, uint32_t mods_latched, uint32_t mods_locked, uint32_t group);
static void keyboard_repeat_info(void *data, struct wl_keyboard *kb, int32_t rate, int32_t delay);
static void pointer_enter(void *data, struct wl_pointer *pt, uint32_t serial,
    struct wl_surface *surf, wl_fixed_t sx, wl_fixed_t sy);
static void pointer_leave(void *data, struct wl_pointer *pt, uint32_t serial,
    struct wl_surface *surf);
static void pointer_motion(void *data, struct wl_pointer *pt, uint32_t time,
    wl_fixed_t sx, wl_fixed_t sy);
static void pointer_button(void *data, struct wl_pointer *pt, uint32_t serial,
    uint32_t time, uint32_t button, uint32_t state);
static void pointer_axis(void *data, struct wl_pointer *pt, uint32_t time,
    uint32_t axis, wl_fixed_t value);
static void pointer_frame(void *data, struct wl_pointer *pt);
static void pointer_axis_source(void *data, struct wl_pointer *pt, uint32_t axis_source);
static void pointer_axis_stop(void *data, struct wl_pointer *pt, uint32_t time, uint32_t axis);
static void pointer_axis_discrete(void *data, struct wl_pointer *pt, uint32_t axis, int32_t discrete);
static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps);
static void seat_name(void *data, struct wl_seat *seat, const char *name);
static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
    const char *interface, uint32_t version);
static void registry_global_remove(void *data, struct wl_registry *reg, uint32_t name);

const struct wl_interface xdg_popup_interface;

static struct wl_display *display;
static struct wl_registry *registry;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct zwp_virtual_keyboard_manager_v1 *vk_mgr;
static struct zwp_virtual_keyboard_v1 *vk;
static struct wl_seat *seat;
static struct wl_surface *surface;
static struct zwlr_layer_surface_v1 *layer_surface;
static struct wl_keyboard *keyboard;
static struct wl_pointer *pointer;

static struct xkb_context *xkb_ctx;
static struct xkb_keymap *xkb_map;
static struct xkb_state *xkb_st;

static struct buffer win_buf;
static cairo_surface_t *orig_surf;
static cairo_surface_t *trans_surf;

static const char *orig_path;
static const char *trans_path;

static int win_width;
static int win_height;
static int configured;
static int show_trans;
static int vk_keymap_set;
static volatile sig_atomic_t running = 1;
static volatile sig_atomic_t reload_requested;

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
	.configure = layer_surface_configure,
	.closed = layer_surface_closed,
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

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

static void
die(const char *msg)
{
	fprintf(stderr, "%s: %s\n", msg, strerror(errno));
	exit(1);
}

static int
create_shm_buffer(int width, int height, struct buffer *b)
{
	int fd;
	size_t sz;
	struct wl_shm_pool *pool;

	sz = (size_t)width * (size_t)height * 4;
	fd = memfd_create("screen-translate-shm", MFD_CLOEXEC);
	if (fd < 0)
		return -1;
	if (ftruncate(fd, sz) < 0) {
		close(fd);
		return -1;
	}

	b->data = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (b->data == MAP_FAILED) {
		close(fd);
		return -1;
	}

	pool = wl_shm_create_pool(shm, fd, sz);
	b->wl_buf = wl_shm_pool_create_buffer(pool, 0, width, height, width * 4,
	    WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);

	b->size = sz;
	b->width = width;
	b->height = height;
	return 0;
}

static void
destroy_shm_buffer(struct buffer *b)
{
	if (b->wl_buf) {
		wl_buffer_destroy(b->wl_buf);
		b->wl_buf = NULL;
	}
	if (b->data && b->data != MAP_FAILED) {
		munmap(b->data, b->size);
		b->data = NULL;
	}
	b->size = 0;
}

static void
load_images(void)
{
	if (!orig_surf && orig_path) {
		orig_surf = cairo_image_surface_create_from_png(orig_path);
		if (cairo_surface_status(orig_surf) != CAIRO_STATUS_SUCCESS) {
			cairo_surface_destroy(orig_surf);
			orig_surf = NULL;
		}
	}
	if (trans_path && access(trans_path, R_OK) == 0) {
		if (trans_surf)
			cairo_surface_destroy(trans_surf);
		trans_surf = cairo_image_surface_create_from_png(trans_path);
		if (cairo_surface_status(trans_surf) != CAIRO_STATUS_SUCCESS) {
			cairo_surface_destroy(trans_surf);
			trans_surf = NULL;
		} else {
			show_trans = 1;
		}
	}
}

static void
render(void)
{
	cairo_surface_t *target_cairo;
	cairo_t *cr;
	cairo_surface_t *src;
	int src_w;
	int src_h;

	if (!configured || !win_buf.data)
		return;

	target_cairo = cairo_image_surface_create_for_data(
	    (unsigned char *)win_buf.data, CAIRO_FORMAT_ARGB32,
	    win_width, win_height, win_width * 4);
	cr = cairo_create(target_cairo);

	cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
	cairo_paint(cr);

	src = (show_trans && trans_surf) ? trans_surf : orig_surf;
	if (src) {
		src_w = cairo_image_surface_get_width(src);
		src_h = cairo_image_surface_get_height(src);
		if (src_w > 0 && src_h > 0) {
			cairo_save(cr);
			cairo_scale(cr, (double)win_width / (double)src_w,
			    (double)win_height / (double)src_h);
			cairo_set_source_surface(cr, src, 0.0, 0.0);
			cairo_paint(cr);
			cairo_restore(cr);
		}
	}

	if (!trans_surf) {
		cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.75);
		cairo_rectangle(cr, (win_width - 200) / 2.0, 24.0, 200.0, 40.0);
		cairo_fill(cr);

		cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
		    CAIRO_FONT_WEIGHT_BOLD);
		cairo_set_font_size(cr, 16.0);
		cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
		cairo_move_to(cr, (win_width - 200) / 2.0 + 45.0, 49.0);
		cairo_show_text(cr, "Translating...");
	}

	cairo_destroy(cr);
	cairo_surface_destroy(target_cairo);

	wl_surface_attach(surface, win_buf.wl_buf, 0, 0);
	wl_surface_damage(surface, 0, 0, win_width, win_height);
	wl_surface_commit(surface);
}

static void
sig_handler(int sig)
{
	if (sig == SIGUSR1)
		reload_requested = 1;
	else
		running = 0;
}

static void
layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *ls,
    uint32_t serial, uint32_t width, uint32_t height)
{
	(void)data;
	zwlr_layer_surface_v1_ack_configure(ls, serial);

	if (win_width != (int)width || win_height != (int)height || !win_buf.wl_buf) {
		win_width = (int)width;
		win_height = (int)height;
		destroy_shm_buffer(&win_buf);
		if (create_shm_buffer(win_width, win_height, &win_buf) < 0)
			die("create_shm_buffer failed");
	}

	configured = 1;
	render();
}

static void
layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *ls)
{
	(void)data;
	(void)ls;
	running = 0;
}

static void
keyboard_keymap(void *data, struct wl_keyboard *kb, uint32_t format,
    int32_t fd, uint32_t size)
{
	char *map_str;

	(void)data;
	(void)kb;
	if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
		close(fd);
		return;
	}

	if (vk && !vk_keymap_set) {
		vk_keymap_set = 1;
		zwp_virtual_keyboard_v1_keymap(vk, format, fd, size);
	}

	map_str = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
	if (map_str == MAP_FAILED) {
		close(fd);
		return;
	}

	if (xkb_st)
		xkb_state_unref(xkb_st);
	if (xkb_map)
		xkb_keymap_unref(xkb_map);

	xkb_map = xkb_keymap_new_from_string(xkb_ctx, map_str,
	    XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
	munmap(map_str, size);
	close(fd);

	if (xkb_map)
		xkb_st = xkb_state_new(xkb_map);
}

static void
keyboard_enter(void *data, struct wl_keyboard *kb, uint32_t serial,
    struct wl_surface *surf, struct wl_array *keys)
{
	(void)data;
	(void)kb;
	(void)serial;
	(void)surf;
	(void)keys;
}

static void
keyboard_leave(void *data, struct wl_keyboard *kb, uint32_t serial,
    struct wl_surface *surf)
{
	(void)data;
	(void)kb;
	(void)serial;
	(void)surf;
}

static void
keyboard_key(void *data, struct wl_keyboard *kb, uint32_t serial,
    uint32_t time, uint32_t key, uint32_t state)
{
	xkb_keysym_t sym;

	(void)data;
	(void)kb;
	(void)serial;

	if (state != WL_KEYBOARD_KEY_STATE_PRESSED || !xkb_st)
		return;

	sym = xkb_state_key_get_one_sym(xkb_st, key + 8);
	if (sym == XKB_KEY_space || sym == XKB_KEY_Tab) {
		show_trans = !show_trans;
		render();
		return;
	}

	if (layer_surface) {
		zwlr_layer_surface_v1_destroy(layer_surface);
		layer_surface = NULL;
	}
	if (surface) {
		wl_surface_destroy(surface);
		surface = NULL;
	}
	wl_display_roundtrip(display);

	if (sym != XKB_KEY_Escape && sym != XKB_KEY_q && sym != XKB_KEY_Q) {
		if (vk) {
			zwp_virtual_keyboard_v1_key(vk, time, key, WL_KEYBOARD_KEY_STATE_PRESSED);
			zwp_virtual_keyboard_v1_key(vk, time, key, WL_KEYBOARD_KEY_STATE_RELEASED);
			wl_display_roundtrip(display);
		}
	}
	running = 0;
}

static void
keyboard_modifiers(void *data, struct wl_keyboard *kb, uint32_t serial,
    uint32_t mods_depressed, uint32_t mods_latched, uint32_t mods_locked, uint32_t group)
{
	(void)data;
	(void)kb;
	(void)serial;
	if (xkb_st)
		xkb_state_update_mask(xkb_st, mods_depressed, mods_latched,
		    mods_locked, 0, 0, group);
	if (vk)
		zwp_virtual_keyboard_v1_modifiers(vk, mods_depressed, mods_latched,
		    mods_locked, group);
}

static void
keyboard_repeat_info(void *data, struct wl_keyboard *kb, int32_t rate, int32_t delay)
{
	(void)data;
	(void)kb;
	(void)rate;
	(void)delay;
}

static void
pointer_enter(void *data, struct wl_pointer *pt, uint32_t serial,
    struct wl_surface *surf, wl_fixed_t sx, wl_fixed_t sy)
{
	(void)data;
	(void)pt;
	(void)serial;
	(void)surf;
	(void)sx;
	(void)sy;
}

static void
pointer_leave(void *data, struct wl_pointer *pt, uint32_t serial,
    struct wl_surface *surf)
{
	(void)data;
	(void)pt;
	(void)serial;
	(void)surf;
}

static void
pointer_motion(void *data, struct wl_pointer *pt, uint32_t time,
    wl_fixed_t sx, wl_fixed_t sy)
{
	(void)data;
	(void)pt;
	(void)time;
	(void)sx;
	(void)sy;
}

static void
pointer_button(void *data, struct wl_pointer *pt, uint32_t serial,
    uint32_t time, uint32_t button, uint32_t state)
{
	(void)data;
	(void)pt;
	(void)serial;
	(void)time;
	(void)button;

	if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
		if (layer_surface) {
			zwlr_layer_surface_v1_destroy(layer_surface);
			layer_surface = NULL;
		}
		if (surface) {
			wl_surface_destroy(surface);
			surface = NULL;
		}
		running = 0;
	}
}

static void
pointer_axis(void *data, struct wl_pointer *pt, uint32_t time,
    uint32_t axis, wl_fixed_t value)
{
	(void)data;
	(void)pt;
	(void)time;
	(void)axis;
	(void)value;
}

static void
pointer_frame(void *data, struct wl_pointer *pt)
{
	(void)data;
	(void)pt;
}

static void
pointer_axis_source(void *data, struct wl_pointer *pt, uint32_t axis_source)
{
	(void)data;
	(void)pt;
	(void)axis_source;
}

static void
pointer_axis_stop(void *data, struct wl_pointer *pt, uint32_t time, uint32_t axis)
{
	(void)data;
	(void)pt;
	(void)time;
	(void)axis;
}

static void
pointer_axis_discrete(void *data, struct wl_pointer *pt, uint32_t axis, int32_t discrete)
{
	(void)data;
	(void)pt;
	(void)axis;
	(void)discrete;
}

static void
seat_capabilities(void *data, struct wl_seat *s, uint32_t caps)
{
	(void)data;
	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !keyboard) {
		keyboard = wl_seat_get_keyboard(s);
		wl_keyboard_add_listener(keyboard, &keyboard_listener, NULL);
	} else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && keyboard) {
		wl_keyboard_destroy(keyboard);
		keyboard = NULL;
	}

	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
		pointer = wl_seat_get_pointer(s);
		wl_pointer_add_listener(pointer, &pointer_listener, NULL);
	} else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && pointer) {
		wl_pointer_destroy(pointer);
		pointer = NULL;
	}
}

static void
seat_name(void *data, struct wl_seat *s, const char *name)
{
	(void)data;
	(void)s;
	(void)name;
}

static void
registry_global(void *data, struct wl_registry *reg, uint32_t name,
    const char *interface, uint32_t version)
{
	(void)data;
	(void)version;
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
		layer_shell = wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, 1);
	} else if (strcmp(interface, zwp_virtual_keyboard_manager_v1_interface.name) == 0) {
		vk_mgr = wl_registry_bind(reg, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		seat = wl_registry_bind(reg, name, &wl_seat_interface, 5);
		wl_seat_add_listener(seat, &seat_listener, NULL);
	}
}

static void
registry_global_remove(void *data, struct wl_registry *reg, uint32_t name)
{
	(void)data;
	(void)reg;
	(void)name;
}

int
main(int argc, char *argv[])
{
	struct sigaction sa;
	struct pollfd pfd;
	int ret;

	if (argc < 2 || strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
		fprintf(stderr, "usage: screen-translate-overlay <orig.png> [trans.png]\n");
		return 1;
	}

	orig_path = argv[1];
	if (argc >= 3)
		trans_path = argv[2];

	xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (!xkb_ctx)
		die("xkb_context_new failed");

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sig_handler;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGUSR1, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);

	display = wl_display_connect(NULL);
	if (!display)
		die("wl_display_connect failed");

	registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);

	if (!compositor || !shm || !layer_shell)
		die("missing required wayland globals");

	if (vk_mgr && seat)
		vk = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(vk_mgr, seat);

	surface = wl_compositor_create_surface(compositor);
	if (!surface)
		die("wl_compositor_create_surface failed");

	layer_surface = zwlr_layer_shell_v1_get_layer_surface(layer_shell,
	    surface, NULL, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "screen-translate");
	if (!layer_surface)
		die("zwlr_layer_shell_v1_get_layer_surface failed");

	zwlr_layer_surface_v1_set_anchor(layer_surface,
	    ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
	    ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
	    ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
	    ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
	zwlr_layer_surface_v1_set_exclusive_zone(layer_surface, -1);
	zwlr_layer_surface_v1_set_keyboard_interactivity(layer_surface,
	    ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
	zwlr_layer_surface_v1_add_listener(layer_surface, &layer_surface_listener, NULL);

	wl_surface_commit(surface);
	load_images();

	pfd.fd = wl_display_get_fd(display);
	pfd.events = POLLIN;

	while (running) {
		while (wl_display_prepare_read(display) != 0) {
			if (wl_display_dispatch_pending(display) < 0) {
				running = 0;
				break;
			}
		}
		if (!running)
			break;

		if (wl_display_flush(display) < 0 && errno != EAGAIN) {
			wl_display_cancel_read(display);
			break;
		}

		pfd.revents = 0;
		ret = poll(&pfd, 1, 100);
		if (ret > 0) {
			if (wl_display_read_events(display) < 0)
				break;
			if (wl_display_dispatch_pending(display) < 0)
				break;
		} else if (ret == 0) {
			wl_display_cancel_read(display);
		} else {
			wl_display_cancel_read(display);
			if (errno != EINTR)
				break;
		}

		if (reload_requested) {
			reload_requested = 0;
			load_images();
			render();
		}
	}

	destroy_shm_buffer(&win_buf);
	if (orig_surf)
		cairo_surface_destroy(orig_surf);
	if (trans_surf)
		cairo_surface_destroy(trans_surf);
	if (vk)
		zwp_virtual_keyboard_v1_destroy(vk);
	if (vk_mgr)
		zwp_virtual_keyboard_manager_v1_destroy(vk_mgr);
	if (keyboard)
		wl_keyboard_destroy(keyboard);
	if (pointer)
		wl_pointer_destroy(pointer);
	if (seat)
		wl_seat_destroy(seat);
	if (layer_surface)
		zwlr_layer_surface_v1_destroy(layer_surface);
	if (surface)
		wl_surface_destroy(surface);
	if (layer_shell)
		zwlr_layer_shell_v1_destroy(layer_shell);
	if (shm)
		wl_shm_destroy(shm);
	if (compositor)
		wl_compositor_destroy(compositor);
	if (registry)
		wl_registry_destroy(registry);
	if (xkb_st)
		xkb_state_unref(xkb_st);
	if (xkb_map)
		xkb_keymap_unref(xkb_map);
	if (xkb_ctx)
		xkb_context_unref(xkb_ctx);
	wl_display_disconnect(display);

	return 0;
}

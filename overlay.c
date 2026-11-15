#define _GNU_SOURCE

#include <cairo/cairo.h>
#include <err.h>
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

#define SHMNAME "screen-translate-shm"

struct buffer {
	struct wl_buffer *wlbuf;
	void *data;
	size_t size;
	int width;
	int height;
};

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

static char *argv0;
static int show_trans;
static volatile sig_atomic_t running = 1;
static volatile sig_atomic_t reload_requested;

static void
usage(void)
{
	fprintf(stderr, "usage: %s orig.png [trans.png]\n", argv0);
	exit(1);
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
destroy_shm_buffer(struct buffer *buf)
{
	if (buf->wlbuf) {
		wl_buffer_destroy(buf->wlbuf);
		buf->wlbuf = NULL;
	}
	if (buf->data) {
		munmap(buf->data, buf->size);
		buf->data = NULL;
	}
	buf->size = 0;
	buf->width = 0;
	buf->height = 0;
}

static int
create_shm_buffer(int width, int height, struct buffer *buf)
{
	struct wl_shm_pool *pool;
	struct wl_buffer *wlbuf;
	void *data;
	size_t sz;
	int fd;

	sz = (size_t)width * (size_t)height * 4;
	if ((fd = memfd_create(SHMNAME, MFD_CLOEXEC)) < 0)
		return -1;
	if (ftruncate(fd, sz) < 0)
		goto close_fd;
	data = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED)
		goto close_fd;
	if (!(pool = wl_shm_create_pool(shm, fd, sz)))
		goto unmap_data;
	wlbuf = wl_shm_pool_create_buffer(pool, 0, width, height, width * 4, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	if (!wlbuf) {
		munmap(data, sz);
		return -1;
	}
	buf->data = data;
	buf->wlbuf = wlbuf;
	buf->size = sz;
	buf->width = width;
	buf->height = height;
	return 0;

unmap_data:
	munmap(data, sz);
close_fd:
	close(fd);
	return -1;
}

static void
unmap_surface(void)
{
	if (!layer_surface && !surface)
		return;
	if (layer_surface) {
		zwlr_layer_surface_v1_destroy(layer_surface);
		layer_surface = NULL;
	}
	if (surface) {
		wl_surface_destroy(surface);
		surface = NULL;
	}
	if (display)
		wl_display_roundtrip(display);
}

static cairo_surface_t *
load_png(const char *path)
{
	cairo_surface_t *s;

	if (!path || !(s = cairo_image_surface_create_from_png(path)))
		return NULL;
	if (cairo_surface_status(s)) {
		cairo_surface_destroy(s);
		return NULL;
	}
	return s;
}

static void
load_trans(const char *path)
{
	cairo_surface_t *s;

	if ((s = load_png(path))) {
		if (trans_surf)
			cairo_surface_destroy(trans_surf);
		trans_surf = s;
		show_trans = 1;
	}
}

static void
render(void)
{
	cairo_surface_t *src, *target;
	cairo_t *cr;
	double x;
	int h, w;

	if (!win_buf.data || !surface)
		return;

	target = cairo_image_surface_create_for_data((unsigned char *)win_buf.data, CAIRO_FORMAT_ARGB32, win_buf.width, win_buf.height, win_buf.width * 4);
	cr = cairo_create(target);

	cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
	cairo_paint(cr);

	src = (show_trans && trans_surf) ? trans_surf : orig_surf;
	if (src) {
		w = cairo_image_surface_get_width(src);
		h = cairo_image_surface_get_height(src);
		if (w > 0 && h > 0) {
			cairo_save(cr);
			cairo_scale(cr, (double)win_buf.width / (double)w, (double)win_buf.height / (double)h);
			cairo_set_source_surface(cr, src, 0.0, 0.0);
			cairo_paint(cr);
			cairo_restore(cr);
		}
	}

	if (!trans_surf) {
		x = (win_buf.width - 200) / 2.0;
		cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.75);
		cairo_rectangle(cr, x, 24.0, 200.0, 40.0);
		cairo_fill(cr);

		cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
		cairo_set_font_size(cr, 16.0);
		cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
		cairo_move_to(cr, x + 45.0, 49.0);
		cairo_show_text(cr, "Translating...");
	}

	cairo_surface_flush(target);
	cairo_destroy(cr);
	cairo_surface_destroy(target);

	wl_surface_attach(surface, win_buf.wlbuf, 0, 0);
	wl_surface_damage(surface, 0, 0, win_buf.width, win_buf.height);
	wl_surface_commit(surface);
}

static void
ls_closed(void *data, struct zwlr_layer_surface_v1 *ls)
{
	(void)data;
	(void)ls;
	running = 0;
}

static void
ls_configure(void *data, struct zwlr_layer_surface_v1 *ls, uint32_t serial, uint32_t width, uint32_t height)
{
	(void)data;
	zwlr_layer_surface_v1_ack_configure(ls, serial);
	if (!width || !height)
		return;
	if (win_buf.width != (int)width || win_buf.height != (int)height || !win_buf.wlbuf) {
		destroy_shm_buffer(&win_buf);
		if (create_shm_buffer((int)width, (int)height, &win_buf) < 0)
			errx(1, "create_shm_buffer failed");
	}
	render();
}

static void
ptr_axis(void *data, struct wl_pointer *pt, uint32_t time, uint32_t axis, wl_fixed_t val)
{
	(void)data;
	(void)pt;
	(void)time;
	(void)axis;
	(void)val;
}

static void
ptr_button(void *data, struct wl_pointer *pt, uint32_t serial, uint32_t time, uint32_t button, uint32_t state)
{
	(void)data;
	(void)pt;
	(void)serial;
	(void)time;
	(void)button;
	if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
		unmap_surface();
		running = 0;
	}
}

static void
ptr_enter(void *data, struct wl_pointer *pt, uint32_t serial, struct wl_surface *surf, wl_fixed_t sx, wl_fixed_t sy)
{
	(void)data;
	(void)pt;
	(void)serial;
	(void)surf;
	(void)sx;
	(void)sy;
}

static void
ptr_leave(void *data, struct wl_pointer *pt, uint32_t serial, struct wl_surface *surf)
{
	(void)data;
	(void)pt;
	(void)serial;
	(void)surf;
}

static void
ptr_motion(void *data, struct wl_pointer *pt, uint32_t time, wl_fixed_t sx, wl_fixed_t sy)
{
	(void)data;
	(void)pt;
	(void)time;
	(void)sx;
	(void)sy;
}

static void
kb_enter(void *data, struct wl_keyboard *kb, uint32_t serial, struct wl_surface *surf, struct wl_array *keys)
{
	(void)data;
	(void)kb;
	(void)serial;
	(void)surf;
	(void)keys;
}

static void
kb_keymap(void *data, struct wl_keyboard *kb, uint32_t format, int32_t fd, uint32_t size)
{
	static int vk_keymap_set;
	char *mapstr;

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
	mapstr = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
	if (mapstr == MAP_FAILED) {
		close(fd);
		return;
	}
	if (xkb_st) {
		xkb_state_unref(xkb_st);
		xkb_st = NULL;
	}
	if (xkb_map) {
		xkb_keymap_unref(xkb_map);
		xkb_map = NULL;
	}
	xkb_map = xkb_keymap_new_from_string(xkb_ctx, mapstr, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
	munmap(mapstr, size);
	close(fd);
	if (xkb_map)
		xkb_st = xkb_state_new(xkb_map);
}

static void
kb_leave(void *data, struct wl_keyboard *kb, uint32_t serial, struct wl_surface *surf)
{
	(void)data;
	(void)kb;
	(void)serial;
	(void)surf;
}

static void
kb_modifiers(void *data, struct wl_keyboard *kb, uint32_t serial, uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group)
{
	(void)data;
	(void)kb;
	(void)serial;
	if (xkb_st)
		xkb_state_update_mask(xkb_st, depressed, latched, locked, 0, 0, group);
	if (vk)
		zwp_virtual_keyboard_v1_modifiers(vk, depressed, latched, locked, group);
}

static void
kb_repeat_info(void *data, struct wl_keyboard *kb, int32_t rate, int32_t delay)
{
	(void)data;
	(void)kb;
	(void)rate;
	(void)delay;
}

static void
kb_key(void *data, struct wl_keyboard *kb, uint32_t serial, uint32_t time, uint32_t key, uint32_t state)
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
	unmap_surface();
	if (sym != XKB_KEY_Escape && sym != XKB_KEY_q && sym != XKB_KEY_Q && vk) {
		zwp_virtual_keyboard_v1_key(vk, time, key, WL_KEYBOARD_KEY_STATE_PRESSED);
		zwp_virtual_keyboard_v1_key(vk, time, key, WL_KEYBOARD_KEY_STATE_RELEASED);
		wl_display_roundtrip(display);
	}
	running = 0;
}

static void
cleanup(void)
{
	unmap_surface();
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
	if (display)
		wl_display_disconnect(display);
}

static void
seat_name(void *data, struct wl_seat *s, const char *name)
{
	(void)data;
	(void)s;
	(void)name;
}

static void
seat_capabilities(void *data, struct wl_seat *s, uint32_t caps)
{
	static const struct wl_keyboard_listener kb_listener = {
		.keymap = kb_keymap,
		.enter = kb_enter,
		.leave = kb_leave,
		.key = kb_key,
		.modifiers = kb_modifiers,
		.repeat_info = kb_repeat_info,
	};
	static const struct wl_pointer_listener pointer_listener = {
		.enter = ptr_enter,
		.leave = ptr_leave,
		.motion = ptr_motion,
		.button = ptr_button,
		.axis = ptr_axis,
	};

	(void)data;
	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !keyboard) {
		keyboard = wl_seat_get_keyboard(s);
		wl_keyboard_add_listener(keyboard, &kb_listener, NULL);
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
reg_global_remove(void *data, struct wl_registry *reg, uint32_t name)
{
	(void)data;
	(void)reg;
	(void)name;
}

static void
reg_global(void *data, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t version)
{
	static const struct wl_seat_listener seat_listener = {
		.capabilities = seat_capabilities,
		.name = seat_name,
	};

	(void)data;
	(void)version;
	if (!strcmp(iface, wl_compositor_interface.name)) {
		compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
	} else if (!strcmp(iface, wl_shm_interface.name)) {
		shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
	} else if (!strcmp(iface, zwlr_layer_shell_v1_interface.name)) {
		layer_shell = wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, 1);
	} else if (!strcmp(iface, zwp_virtual_keyboard_manager_v1_interface.name)) {
		vk_mgr = wl_registry_bind(reg, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
	} else if (!strcmp(iface, wl_seat_interface.name)) {
		seat = wl_registry_bind(reg, name, &wl_seat_interface, 1);
		wl_seat_add_listener(seat, &seat_listener, NULL);
	}
}

int
main(int argc, char *argv[])
{
	static const struct zwlr_layer_surface_v1_listener layer_listener = {
		.configure = ls_configure,
		.closed = ls_closed,
	};
	static const struct wl_registry_listener reg_listener = {
		.global = reg_global,
		.global_remove = reg_global_remove,
	};
	struct sigaction sa;
	struct pollfd pfd;
	const char *orig_path, *trans_path;
	int ch, ret;

	argv0 = argv[0];
	while ((ch = getopt(argc, argv, "h")) >= 0) {
		switch (ch) {
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;
	if (!argc || argc > 2)
		usage();

	orig_path = argv[0];
	trans_path = (argc > 1) ? argv[1] : NULL;

	if (!(orig_surf = load_png(orig_path)))
		errx(1, "failed to load %s", orig_path);
	if (trans_path)
		load_trans(trans_path);

	if (!(display = wl_display_connect(NULL)))
		errx(1, "wl_display_connect failed");

	if (!(xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS)))
		errx(1, "xkb_context_new failed");

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sig_handler;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGUSR1, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);

	registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &reg_listener, NULL);
	wl_display_roundtrip(display);

	if (!compositor || !shm || !layer_shell)
		errx(1, "missing required wayland globals");

	if (vk_mgr && seat)
		vk = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(vk_mgr, seat);

	if (!(surface = wl_compositor_create_surface(compositor)))
		errx(1, "wl_compositor_create_surface failed");

	layer_surface = zwlr_layer_shell_v1_get_layer_surface(layer_shell, surface, NULL, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "screen-translate");
	if (!layer_surface)
		errx(1, "zwlr_layer_shell_v1_get_layer_surface failed");

	zwlr_layer_surface_v1_set_anchor(layer_surface, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
	zwlr_layer_surface_v1_set_exclusive_zone(layer_surface, -1);
	zwlr_layer_surface_v1_set_keyboard_interactivity(layer_surface, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
	zwlr_layer_surface_v1_add_listener(layer_surface, &layer_listener, NULL);

	wl_surface_commit(surface);

	pfd.fd = wl_display_get_fd(display);
	pfd.events = POLLIN;

	while (running) {
		while (wl_display_prepare_read(display) < 0) {
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
			if (wl_display_read_events(display) < 0 || wl_display_dispatch_pending(display) < 0)
				break;
		} else {
			wl_display_cancel_read(display);
			if (ret < 0 && errno != EINTR)
				break;
		}

		if (reload_requested) {
			reload_requested = 0;
			load_trans(trans_path);
			render();
		}
	}

	cleanup();
	return 0;
}

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
};

static void cleanup(void);
static int createshmbuffer(int width, int height, struct buffer *buf);
static void destroyshmbuffer(struct buffer *buf);
static void kbenter(void *data, struct wl_keyboard *kb, uint32_t serial,
    struct wl_surface *surf, struct wl_array *keys);
static void kbkey(void *data, struct wl_keyboard *kb, uint32_t serial,
    uint32_t time, uint32_t key, uint32_t state);
static void kbkeymap(void *data, struct wl_keyboard *kb, uint32_t format,
    int32_t fd, uint32_t size);
static void kbleave(void *data, struct wl_keyboard *kb, uint32_t serial,
    struct wl_surface *surf);
static void kbmodifiers(void *data, struct wl_keyboard *kb, uint32_t serial,
    uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group);
static void kbrepeatinfo(void *data, struct wl_keyboard *kb, int32_t rate,
    int32_t delay);
static cairo_surface_t *loadpng(const char *path);
static void loadimages(void);
static void lsclosed(void *data, struct zwlr_layer_surface_v1 *ls);
static void lsconfigure(void *data, struct zwlr_layer_surface_v1 *ls,
    uint32_t serial, uint32_t width, uint32_t height);
static void ptraxis(void *data, struct wl_pointer *pt, uint32_t time,
    uint32_t axis, wl_fixed_t val);
static void ptrbutton(void *data, struct wl_pointer *pt, uint32_t serial,
    uint32_t time, uint32_t button, uint32_t state);
static void ptrenter(void *data, struct wl_pointer *pt, uint32_t serial,
    struct wl_surface *surf, wl_fixed_t sx, wl_fixed_t sy);
static void ptrleave(void *data, struct wl_pointer *pt, uint32_t serial,
    struct wl_surface *surf);
static void ptrmotion(void *data, struct wl_pointer *pt, uint32_t time,
    wl_fixed_t sx, wl_fixed_t sy);
static void regglobal(void *data, struct wl_registry *reg, uint32_t name,
    const char *iface, uint32_t version);
static void regglobalremove(void *data, struct wl_registry *reg, uint32_t name);
static void render(void);
static void seatcapabilities(void *data, struct wl_seat *s, uint32_t caps);
static void seatname(void *data, struct wl_seat *s, const char *name);
static void sighandler(int sig);
static void unmapsurface(void);
static void usage(void);

const struct wl_interface xdg_popup_interface;

static struct wl_display *display;
static struct wl_registry *registry;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct zwlr_layer_shell_v1 *layershell;
static struct zwp_virtual_keyboard_manager_v1 *vkmgr;
static struct zwp_virtual_keyboard_v1 *vk;
static struct wl_seat *seat;
static struct wl_surface *surface;
static struct zwlr_layer_surface_v1 *layersurface;
static struct wl_keyboard *keyboard;
static struct wl_pointer *pointer;

static struct xkb_context *xkbctx;
static struct xkb_keymap *xkbmap;
static struct xkb_state *xkbst;

static struct buffer winbuf;
static cairo_surface_t *origsurf;
static cairo_surface_t *transsurf;

static const char *origpath;
static const char *transpath;
static char *argv0;

static int winwidth;
static int winheight;
static int configured;
static int showtrans;
static int vkkeymapset;
static volatile sig_atomic_t running = 1;
static volatile sig_atomic_t reloadrequested;

static const struct zwlr_layer_surface_v1_listener layerlistener = {
	.configure = lsconfigure,
	.closed = lsclosed,
};

static const struct wl_keyboard_listener kblistener = {
	.keymap = kbkeymap,
	.enter = kbenter,
	.leave = kbleave,
	.key = kbkey,
	.modifiers = kbmodifiers,
	.repeat_info = kbrepeatinfo,
};

static const struct wl_pointer_listener pointerlistener = {
	.enter = ptrenter,
	.leave = ptrleave,
	.motion = ptrmotion,
	.button = ptrbutton,
	.axis = ptraxis,
};

static const struct wl_seat_listener seatlistener = {
	.capabilities = seatcapabilities,
	.name = seatname,
};

static const struct wl_registry_listener reglistener = {
	.global = regglobal,
	.global_remove = regglobalremove,
};

static void
cleanup(void)
{
	destroyshmbuffer(&winbuf);
	if (origsurf)
		cairo_surface_destroy(origsurf);
	if (transsurf)
		cairo_surface_destroy(transsurf);
	if (vk)
		zwp_virtual_keyboard_v1_destroy(vk);
	if (vkmgr)
		zwp_virtual_keyboard_manager_v1_destroy(vkmgr);
	if (keyboard)
		wl_keyboard_destroy(keyboard);
	if (pointer)
		wl_pointer_destroy(pointer);
	if (seat)
		wl_seat_destroy(seat);
	unmapsurface();
	if (layershell)
		zwlr_layer_shell_v1_destroy(layershell);
	if (shm)
		wl_shm_destroy(shm);
	if (compositor)
		wl_compositor_destroy(compositor);
	if (registry)
		wl_registry_destroy(registry);
	if (xkbst)
		xkb_state_unref(xkbst);
	if (xkbmap)
		xkb_keymap_unref(xkbmap);
	if (xkbctx)
		xkb_context_unref(xkbctx);
	if (display)
		wl_display_disconnect(display);
}

static int
createshmbuffer(int width, int height, struct buffer *buf)
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
		goto closefd;
	data = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED)
		goto closefd;
	pool = wl_shm_create_pool(shm, fd, sz);
	wlbuf = wl_shm_pool_create_buffer(pool, 0, width, height, width * 4,
	    WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	if (!wlbuf) {
		munmap(data, sz);
		return -1;
	}
	buf->data = data;
	buf->wlbuf = wlbuf;
	buf->size = sz;
	return 0;

closefd:
	close(fd);
	return -1;
}

static void
destroyshmbuffer(struct buffer *buf)
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
}

static void
kbenter(void *data, struct wl_keyboard *kb, uint32_t serial,
    struct wl_surface *surf, struct wl_array *keys)
{
	(void)data;
	(void)kb;
	(void)serial;
	(void)surf;
	(void)keys;
}

static void
kbkey(void *data, struct wl_keyboard *kb, uint32_t serial,
    uint32_t time, uint32_t key, uint32_t state)
{
	xkb_keysym_t sym;

	(void)data;
	(void)kb;
	(void)serial;
	if (state != WL_KEYBOARD_KEY_STATE_PRESSED || !xkbst)
		return;
	sym = xkb_state_key_get_one_sym(xkbst, key + 8);
	if (sym == XKB_KEY_space || sym == XKB_KEY_Tab) {
		showtrans = !showtrans;
		render();
		return;
	}
	unmapsurface();
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
kbkeymap(void *data, struct wl_keyboard *kb, uint32_t format,
    int32_t fd, uint32_t size)
{
	char *mapstr;

	(void)data;
	(void)kb;
	if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
		close(fd);
		return;
	}
	if (vk && !vkkeymapset) {
		vkkeymapset = 1;
		zwp_virtual_keyboard_v1_keymap(vk, format, fd, size);
	}
	mapstr = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
	if (mapstr == MAP_FAILED) {
		close(fd);
		return;
	}
	if (xkbst)
		xkb_state_unref(xkbst);
	if (xkbmap)
		xkb_keymap_unref(xkbmap);
	xkbmap = xkb_keymap_new_from_string(xkbctx, mapstr,
	    XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
	munmap(mapstr, size);
	close(fd);
	if (xkbmap)
		xkbst = xkb_state_new(xkbmap);
}

static void
kbleave(void *data, struct wl_keyboard *kb, uint32_t serial,
    struct wl_surface *surf)
{
	(void)data;
	(void)kb;
	(void)serial;
	(void)surf;
}

static void
kbmodifiers(void *data, struct wl_keyboard *kb, uint32_t serial,
    uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group)
{
	(void)data;
	(void)kb;
	(void)serial;
	if (xkbst)
		xkb_state_update_mask(xkbst, depressed, latched, locked, 0, 0, group);
	if (vk)
		zwp_virtual_keyboard_v1_modifiers(vk, depressed, latched, locked, group);
}

static void
kbrepeatinfo(void *data, struct wl_keyboard *kb, int32_t rate, int32_t delay)
{
	(void)data;
	(void)kb;
	(void)rate;
	(void)delay;
}

static cairo_surface_t *
loadpng(const char *path)
{
	cairo_surface_t *s;

	if (!path || !(s = cairo_image_surface_create_from_png(path)))
		return NULL;
	if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(s);
		return NULL;
	}
	return s;
}

static void
loadimages(void)
{
	cairo_surface_t *s;

	if (!origsurf)
		origsurf = loadpng(origpath);
	if ((s = loadpng(transpath))) {
		if (transsurf)
			cairo_surface_destroy(transsurf);
		transsurf = s;
		showtrans = 1;
	}
}

static void
lsclosed(void *data, struct zwlr_layer_surface_v1 *ls)
{
	(void)data;
	(void)ls;
	running = 0;
}

static void
lsconfigure(void *data, struct zwlr_layer_surface_v1 *ls,
    uint32_t serial, uint32_t width, uint32_t height)
{
	(void)data;
	zwlr_layer_surface_v1_ack_configure(ls, serial);
	if (winwidth != (int)width || winheight != (int)height || !winbuf.wlbuf) {
		winwidth = (int)width;
		winheight = (int)height;
		destroyshmbuffer(&winbuf);
		if (createshmbuffer(winwidth, winheight, &winbuf) < 0)
			errx(1, "createshmbuffer failed");
	}
	configured = 1;
	render();
}

static void
ptraxis(void *data, struct wl_pointer *pt, uint32_t time,
    uint32_t axis, wl_fixed_t val)
{
	(void)data;
	(void)pt;
	(void)time;
	(void)axis;
	(void)val;
}

static void
ptrbutton(void *data, struct wl_pointer *pt, uint32_t serial,
    uint32_t time, uint32_t button, uint32_t state)
{
	(void)data;
	(void)pt;
	(void)serial;
	(void)time;
	(void)button;
	if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
		unmapsurface();
		running = 0;
	}
}

static void
ptrenter(void *data, struct wl_pointer *pt, uint32_t serial,
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
ptrleave(void *data, struct wl_pointer *pt, uint32_t serial,
    struct wl_surface *surf)
{
	(void)data;
	(void)pt;
	(void)serial;
	(void)surf;
}

static void
ptrmotion(void *data, struct wl_pointer *pt, uint32_t time,
    wl_fixed_t sx, wl_fixed_t sy)
{
	(void)data;
	(void)pt;
	(void)time;
	(void)sx;
	(void)sy;
}

static void
regglobal(void *data, struct wl_registry *reg, uint32_t name,
    const char *iface, uint32_t version)
{
	(void)data;
	(void)version;
	if (!strcmp(iface, wl_compositor_interface.name)) {
		compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
	} else if (!strcmp(iface, wl_shm_interface.name)) {
		shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
	} else if (!strcmp(iface, zwlr_layer_shell_v1_interface.name)) {
		layershell = wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, 1);
	} else if (!strcmp(iface, zwp_virtual_keyboard_manager_v1_interface.name)) {
		vkmgr = wl_registry_bind(reg, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
	} else if (!strcmp(iface, wl_seat_interface.name)) {
		seat = wl_registry_bind(reg, name, &wl_seat_interface, 1);
		wl_seat_add_listener(seat, &seatlistener, NULL);
	}
}

static void
regglobalremove(void *data, struct wl_registry *reg, uint32_t name)
{
	(void)data;
	(void)reg;
	(void)name;
}

static void
render(void)
{
	cairo_surface_t *src, *target;
	cairo_t *cr;
	double x;
	int h, w;

	if (!configured || !winbuf.data)
		return;

	target = cairo_image_surface_create_for_data(
	    (unsigned char *)winbuf.data, CAIRO_FORMAT_ARGB32,
	    winwidth, winheight, winwidth * 4);
	cr = cairo_create(target);

	cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
	cairo_paint(cr);

	src = (showtrans && transsurf) ? transsurf : origsurf;
	if (src) {
		w = cairo_image_surface_get_width(src);
		h = cairo_image_surface_get_height(src);
		if (w > 0 && h > 0) {
			cairo_save(cr);
			cairo_scale(cr, (double)winwidth / (double)w,
			    (double)winheight / (double)h);
			cairo_set_source_surface(cr, src, 0.0, 0.0);
			cairo_paint(cr);
			cairo_restore(cr);
		}
	}

	if (!transsurf) {
		x = (winwidth - 200) / 2.0;
		cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.75);
		cairo_rectangle(cr, x, 24.0, 200.0, 40.0);
		cairo_fill(cr);

		cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
		    CAIRO_FONT_WEIGHT_BOLD);
		cairo_set_font_size(cr, 16.0);
		cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
		cairo_move_to(cr, x + 45.0, 49.0);
		cairo_show_text(cr, "Translating...");
	}

	cairo_surface_flush(target);
	cairo_destroy(cr);
	cairo_surface_destroy(target);

	wl_surface_attach(surface, winbuf.wlbuf, 0, 0);
	wl_surface_damage(surface, 0, 0, winwidth, winheight);
	wl_surface_commit(surface);
}

static void
seatcapabilities(void *data, struct wl_seat *s, uint32_t caps)
{
	(void)data;
	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !keyboard) {
		keyboard = wl_seat_get_keyboard(s);
		wl_keyboard_add_listener(keyboard, &kblistener, NULL);
	} else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && keyboard) {
		wl_keyboard_destroy(keyboard);
		keyboard = NULL;
	}

	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
		pointer = wl_seat_get_pointer(s);
		wl_pointer_add_listener(pointer, &pointerlistener, NULL);
	} else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && pointer) {
		wl_pointer_destroy(pointer);
		pointer = NULL;
	}
}

static void
seatname(void *data, struct wl_seat *s, const char *name)
{
	(void)data;
	(void)s;
	(void)name;
}

static void
sighandler(int sig)
{
	if (sig == SIGUSR1)
		reloadrequested = 1;
	else
		running = 0;
}

static void
unmapsurface(void)
{
	if (layersurface) {
		zwlr_layer_surface_v1_destroy(layersurface);
		layersurface = NULL;
	}
	if (surface) {
		wl_surface_destroy(surface);
		surface = NULL;
	}
	if (display)
		wl_display_roundtrip(display);
}

static void
usage(void)
{
	fprintf(stderr, "usage: %s orig.png [trans.png]\n", argv0);
	exit(1);
}

int
main(int argc, char *argv[])
{
	struct sigaction sa;
	struct pollfd pfd;
	int ch, ret;

	argv0 = argv[0];
	while ((ch = getopt(argc, argv, "h")) != -1) {
		switch (ch) {
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;
	if (!argc)
		usage();

	origpath = argv[0];
	if (argc > 1)
		transpath = argv[1];

	if (!(display = wl_display_connect(NULL)))
		errx(1, "wl_display_connect failed");

	if (!(xkbctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS)))
		errx(1, "xkb_context_new failed");

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sighandler;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGUSR1, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);

	registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &reglistener, NULL);
	wl_display_roundtrip(display);

	if (!compositor || !shm || !layershell)
		errx(1, "missing required wayland globals");

	if (vkmgr && seat)
		vk = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(vkmgr, seat);

	if (!(surface = wl_compositor_create_surface(compositor)))
		errx(1, "wl_compositor_create_surface failed");

	layersurface = zwlr_layer_shell_v1_get_layer_surface(layershell,
	    surface, NULL, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "screen-translate");
	if (!layersurface)
		errx(1, "zwlr_layer_shell_v1_get_layer_surface failed");

	zwlr_layer_surface_v1_set_anchor(layersurface,
	    ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
	    ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
	    ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
	    ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
	zwlr_layer_surface_v1_set_exclusive_zone(layersurface, -1);
	zwlr_layer_surface_v1_set_keyboard_interactivity(layersurface,
	    ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
	zwlr_layer_surface_v1_add_listener(layersurface, &layerlistener, NULL);

	wl_surface_commit(surface);
	loadimages();

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
			if (wl_display_read_events(display) < 0 ||
			    wl_display_dispatch_pending(display) < 0)
				break;
		} else {
			wl_display_cancel_read(display);
			if (ret < 0 && errno != EINTR)
				break;
		}

		if (reloadrequested) {
			reloadrequested = 0;
			loadimages();
			render();
		}
	}

	cleanup();
	return 0;
}


#include <argp.h>
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <threads.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include <pixman.h>

#include <rfb/rfb.h>
#include <uv.h>

#include "wayland-virtual-keyboard-client-protocol.h"
#include "wayland-wlr-screencopy-client-protocol.h"
#include "wayland-xdg-output-client-protocol.h"

#include "wvnc.h"
#include "buffer.h"
#include "uinput.h"
#include "utils.h"
#include "damage.h"

struct wvnc_args {
	const char *output;
	in_addr_t address;
	int port;
	int period;
	bool no_uinput;
};

struct wvnc_xkb {
	struct xkb_context *ctx;
	struct xkb_keymap *map;
	struct xkb_state *state;
};

enum wvnc_capture_state {
	WVNC_STATE_IDLE = 0,
	WVNC_STATE_CAPTURING,
};

struct wvnc_client;

struct wvnc {
	struct {
		rfbScreenInfo *screen_info;
		rgba_t *fb;
	} rfb;
	struct {
		struct wl_display *display;
		struct wl_registry *registry;
		struct wl_shm *shm;
		struct zxdg_output_manager_v1 *output_manager;
		struct zwlr_screencopy_manager_v1 *screencopy_manager;
		struct zwp_virtual_keyboard_manager_v1 *keyboard_manager;
		struct zwp_virtual_keyboard_v1 *keyboard;
	} wl;

	struct wvnc_xkb xkb;
	struct wvnc_args args;
	struct wvnc_uinput uinput;
	struct wvnc_buffer buffers[2];
	unsigned int buffer_i;

	struct wl_list outputs;
	struct wvnc_output *selected_output;
	struct wl_list seats;
	struct wvnc_seat *selected_seat;

	uint32_t logical_width;
	uint32_t logical_height;

	uv_loop_t main_loop;
	uv_poll_t wayland_poller;
	uv_poll_t rfb_poller;
	uv_prepare_t flusher;

	struct wl_list clients;
	uv_timer_t capture_timer;
	enum wvnc_capture_state capture_state;
	int is_first_capture_done;
};

struct wvnc_client {
	struct wl_list link;
	struct wvnc* wvnc;
	rfbClientPtr rfb_client;
	uv_poll_t handle;
};

// This is because we can't pass our global pointer into some of the 
// rfb callbacks. Use minimally.
thread_local struct wvnc *global_wvnc;


static void initialize_shm_buffer(struct wvnc_buffer *buffer,
								  enum wl_shm_format format,
								  uint32_t width, uint32_t height,
								  uint32_t stride)
{
	int fd = shm_create();
	size_t size = stride * height;
	int ret = ftruncate(fd, size);
	if (ret < 0) {
		fail("Failed to allocate shm buffer");
	}

	void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		fail("mmap failed");
	}

	struct wl_shm_pool *pool = wl_shm_create_pool(buffer->wvnc->wl.shm, fd, size);
	close(fd);
	struct wl_buffer *wl_buffer = wl_shm_pool_create_buffer(
		pool, 0, width, height, stride, format
	);
	wl_shm_pool_destroy(pool);

	buffer->wl = wl_buffer;
	buffer->data = data;
	buffer->width = width;
	buffer->height = height;
	buffer->size = size;
	buffer->format = format;
	buffer->stride = stride;
}


static void handle_output_geometry(void *data, struct wl_output *wl,
								   int32_t x, int32_t y,
								   int32_t p_w, int32_t p_h,
								   int32_t subp, const char *make,
								   const char *model,
								   int32_t transform)
{
	struct wvnc_output *output = data;
	output->transform = transform;
}


static void handle_output_mode(void *data, struct wl_output *wl,
							   uint32_t flags, int32_t width, int32_t height,
							   int32_t refresh)
{
}


static void handle_output_scale(void *data, struct wl_output *wl, int32_t factor)
{
	// TODO: Do we care?
}


static void handle_output_done(void *data, struct wl_output *wl)
{
}


struct wl_output_listener output_listener = {
	.geometry = handle_output_geometry,
	.mode = handle_output_mode,
	.done = handle_output_done,
	.scale = handle_output_scale,
};


static void handle_frame_buffer(void *data,
								struct zwlr_screencopy_frame_v1 *frame,
								enum wl_shm_format format, uint32_t width,
								uint32_t height, uint32_t stride)
{
	struct wvnc_buffer *buffer = data;
	// TODO: What if input size changes or something?
	if (buffer->wl == NULL) {
		initialize_shm_buffer(buffer, format, width, height, stride);
	}
	zwlr_screencopy_frame_v1_copy(frame, buffer->wl);
}


static void handle_frame_flags(void *data,
							   struct zwlr_screencopy_frame_v1 *frame,
							   uint32_t flags)
{
	// TODO: Handle Y-inversion here? (the only flag currently defined)
	struct wvnc_buffer *buffer = data;
	if (flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT) {
		buffer->y_invert = true;
	}
}


static void handle_frame_ready(void *data,
							   struct zwlr_screencopy_frame_v1 *frame,
							   uint32_t tv_sec_hi, uint32_t tv_sec_lo,
							   uint32_t tv_nsec)
{
	struct wvnc_buffer *buffer = data;
	buffer->done = true;
}

static void handle_frame_failed(void *data,
								struct zwlr_screencopy_frame_v1 *frame)
{
	fail("Failed to capture output");
}


static const struct zwlr_screencopy_frame_v1_listener frame_listener = {
	.buffer = handle_frame_buffer,
	.flags = handle_frame_flags,
	.ready = handle_frame_ready,
	.failed = handle_frame_failed
};


static void handle_xdg_output_logical_position(void *data,
											   struct zxdg_output_v1 *xdg,
											   int32_t x, int32_t y)
{
	struct wvnc_output *output = data;
	output->x = x;
	output->y = y;
}


static void handle_xdg_output_logical_size(void *data,
										   struct zxdg_output_v1 *xdg,
										   int32_t width, int32_t height)
{
	struct wvnc_output *output = data;
	// Note that this is _logical size_
	// That is, it includes rotations and scalings
	output->width = width;
	output->height = height;
}


static void handle_xdg_output_done(void *data, struct zxdg_output_v1 *xdg)
{
}


static void handle_xdg_output_name(void *data, struct zxdg_output_v1 *xdg,
								   const char *name)
{
	struct wvnc_output *output = data;
	output->name = strdup(name);
}


static void handle_xdg_output_description(void *data, struct zxdg_output_v1 *xdg,
										  const char *description)
{
}


static const struct zxdg_output_v1_listener xdg_output_listener = {
	.logical_position = handle_xdg_output_logical_position,
	.logical_size = handle_xdg_output_logical_size,
	.done = handle_xdg_output_done,
	.name = handle_xdg_output_name,
	.description = handle_xdg_output_description,
};


static void handle_seat_capabilities(void *data, struct wl_seat *wl_seat,
									 uint32_t capabilities)
{
	struct wvnc_seat *seat = data;
	seat->capabilities = capabilities;
}


static void handle_seat_name(void *data, struct wl_seat *wl_seat,
							 const char *name)
{
	struct wvnc_seat *seat = data;
	seat->name = strdup(name);
	log_info("Name = %s", seat->name);
}


static const struct wl_seat_listener wl_seat_listener = {
	.capabilities = handle_seat_capabilities,
	.name = handle_seat_name,
};


static void handle_wl_registry_global(void *data, struct wl_registry *registry,
									  uint32_t name, const char *interface,
									  uint32_t version)
{
#define IS_PROTOCOL(x) !strcmp(interface, x##_interface.name)
#define BIND(x, ver) wl_registry_bind(registry, name, &x##_interface, min((uint32_t)ver, version))
	struct wvnc *wvnc = data;
	if (IS_PROTOCOL(wl_output)) {
		struct wvnc_output *out = xmalloc(sizeof(struct wvnc_output));
		out->wl = BIND(wl_output, 1);
		wl_output_add_listener(out->wl, &output_listener, out);
		wl_list_insert(&wvnc->outputs, &out->link);
	} else if (IS_PROTOCOL(zxdg_output_manager_v1)) {
		wvnc->wl.output_manager = BIND(zxdg_output_manager_v1, 2);
	} else if (IS_PROTOCOL(zwlr_screencopy_manager_v1)) {
		wvnc->wl.screencopy_manager = BIND(zwlr_screencopy_manager_v1, 1);
	} else if (IS_PROTOCOL(wl_shm)) {
		wvnc->wl.shm = BIND(wl_shm, 1);
	} else if (IS_PROTOCOL(wl_seat)) {
		// This is the seat we bind our virtual keyboard to
		// sway currently does not support binding somewhere else than seat0,
		// so we don't support choosing here either.
		// TODO: Patch sway and fix this
		struct wvnc_seat *seat = xmalloc(sizeof(struct wvnc_seat));
		seat->wl = BIND(wl_seat, 7);
		wl_seat_add_listener(seat->wl, &wl_seat_listener, seat);
		wl_list_insert(&wvnc->seats, &seat->link);
	} else if (IS_PROTOCOL(zwp_virtual_keyboard_manager_v1)) {
		wvnc->wl.keyboard_manager = BIND(zwp_virtual_keyboard_manager_v1, 1);
	}
#undef BIND
#undef IS_PROTOCOL
}


static void handle_wl_registry_global_remove(void *data,
											 struct wl_registry *registry,
											 uint32_t name)
{
}


static const struct wl_registry_listener registry_listener = {
	.global = handle_wl_registry_global,
	.global_remove = handle_wl_registry_global_remove,
};

void destroy_client(struct wvnc_client *client)
{
	uv_poll_stop(&client->handle);
	wl_list_remove(&client->link);
	free(client);
}

void handle_client_event(uv_poll_t *handle, int status, int event)
{
	struct wvnc_client* client = wl_container_of(handle, client, handle);

	if (event & UV_DISCONNECT)
		destroy_client(client);

	rfbProcessEvents(client->wvnc->rfb.screen_info, 0);
}

static enum rfbNewClientAction rfb_new_client_hook(rfbClientPtr cl)
{
	struct wvnc_client *self = calloc(1, sizeof(*self));
	if (!self)
		return RFB_CLIENT_REFUSE;

	self->rfb_client = cl;
	self->wvnc = global_wvnc;
	wl_list_insert(&self->wvnc->clients, &self->link);

	uv_poll_init(&self->wvnc->main_loop, &self->handle, cl->sock);
	uv_poll_start(&self->handle, UV_READABLE | UV_PRIORITIZED |
		      UV_DISCONNECT, handle_client_event);

	cl->clientData = self;

	return RFB_CLIENT_ACCEPT;
}


static void rfb_ptr_hook(int mask, int screen_x, int screen_y, rfbClientPtr cl)
{
	struct wvnc_client *client = cl->clientData;
	struct wvnc *wvnc = client->wvnc;

	if (!wvnc->uinput.initialized) {
		return; // Nothing to do here
	}
	// Way too lazy to debug fixpoing scaling
	float global_x = (float)wvnc->selected_output->x +
		clamp(screen_x, 0, (int)wvnc->selected_output->width);
	float global_y = (float)wvnc->selected_output->y +
		clamp(screen_y, 0, (int)wvnc->selected_output->height);
	int32_t touch_x = round(global_x / wvnc->logical_width * UINPUT_ABS_MAX);
	int32_t touch_y = round(global_y / wvnc->logical_height * UINPUT_ABS_MAX);

	uinput_move_abs(&wvnc->uinput, (int32_t)touch_x, (int32_t)touch_y);

	uinput_set_buttons(
		&wvnc->uinput,
		!!(mask & BIT(0)), !!(mask & BIT(1)), !!(mask & BIT(2))
	);

	if (mask & BIT(4)) {
		uinput_wheel(&wvnc->uinput, false);
	}
	if (mask & BIT(3)) {
		uinput_wheel(&wvnc->uinput, true);
	}
}


struct key_iter_search {
	xkb_keysym_t keysym;

	xkb_keycode_t keycode;
	xkb_level_index_t level;
};

	
static void key_iter(struct xkb_keymap *xkb, xkb_keycode_t key, void *data) {
	struct key_iter_search *search = data;
	if (search->keycode != XKB_KEYCODE_INVALID) {
		return;  // We are done
	}
	xkb_level_index_t num_levels = xkb_keymap_num_levels_for_key(xkb, key, 0);
	for (xkb_level_index_t i = 0; i < num_levels; i++) {
		const xkb_keysym_t *syms;
		int num_syms = xkb_keymap_key_get_syms_by_level(xkb, key, 0, i, &syms);
		for (int k = 0; k < num_syms; k++) {
			if (syms[k] == search->keysym) {
				search->keycode = key;
				search->level = i;
				break;
				goto end;
			}
		}
	}
end:
	return;
}

static void rfb_key_hook(rfbBool down, rfbKeySym keysym, rfbClientPtr cl)
{
	struct wvnc_client *client = cl->clientData;
	struct wvnc *wvnc = client->wvnc;

	struct wvnc_xkb *xkb = &wvnc->xkb;
	if (wvnc->wl.keyboard == NULL) {
		return;
	}

	struct key_iter_search search = {
		.keysym = keysym,
		.keycode = XKB_KEYCODE_INVALID,
		.level = 0,
	};
	xkb_keymap_key_for_each(xkb->map, key_iter, &search);
	if (search.keycode == XKB_KEYCODE_INVALID) {
		log_error("Keysym %04x not found in our keymap", keysym);
		return;
	}

	zwp_virtual_keyboard_v1_key(
		wvnc->wl.keyboard, 0,
		search.keycode - xkb_keymap_min_keycode(xkb->map) + 1,
		down ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED
	);
	wl_display_dispatch_pending(wvnc->wl.display);

	enum xkb_state_component component =
		xkb_state_update_key(xkb->state, search.keycode,
							 down ? XKB_KEY_DOWN : XKB_KEY_UP);

	if (component & (XKB_STATE_MODS_DEPRESSED | XKB_STATE_MODS_LATCHED |
					 XKB_STATE_MODS_LOCKED | XKB_STATE_MODS_EFFECTIVE)) {
		// Modifiers changed, we have to propagate them to the compositor
		xkb_mod_mask_t depressed = xkb_state_serialize_mods(
			xkb->state, XKB_STATE_MODS_DEPRESSED
		);
		xkb_mod_mask_t latched = xkb_state_serialize_mods(
			xkb->state, XKB_STATE_MODS_LATCHED
		);
		xkb_mod_mask_t locked = xkb_state_serialize_mods(
			xkb->state, XKB_STATE_MODS_LOCKED
		);
		xkb_mod_mask_t group = xkb_state_serialize_mods(
			xkb->state, XKB_STATE_MODS_EFFECTIVE
		);

		zwp_virtual_keyboard_v1_modifiers(
			wvnc->wl.keyboard, depressed, latched, locked, group
		);
	}
}


static void update_framebuffer_full(struct wvnc *wvnc, struct wvnc_buffer *new)
{
	buffer_to_fb(wvnc->rfb.fb, wvnc->selected_output, new,
				 0, 0, new->width, new->height);
	rfbMarkRectAsModified(
		wvnc->rfb.screen_info,
		0, 0, wvnc->selected_output->width, wvnc->selected_output->height
	);
}

static void update_framebuffer_with_damage_check(struct wvnc *wvnc,
							   struct wvnc_buffer *old, struct wvnc_buffer *new)
{
	assert(new->width == old->width && new->height == old->height &&
		   new->stride == old->stride);

	struct pixman_region32 region;
	pixman_region32_init(&region);

	struct bitmap* damage = damage_compute(old->data, new->data, new->width, new->height);
	if (!damage || bitmap_is_empty(damage))
		goto done;

	damage_to_pixman(&region, damage, new->width, new->height);

	struct pixman_box32 *box = pixman_region32_extents(&region);
	int box_width = box->x2 - box->x1;
	int box_height = box->y2 - box->y1;

	buffer_to_fb(wvnc->rfb.fb, wvnc->selected_output, new, box->x1, box->y1,
			box_width, box_height);
	rfbMarkRectAsModified(wvnc->rfb.screen_info, box->x1, new->height - box->y2, box->x2,
			new->height - box->y1);

done:
	pixman_region32_fini(&region);
	free(damage);
}


static void calculate_logical_size(struct wvnc *wvnc)
{
	int32_t min_x = INT32_MAX;
	int32_t max_x = INT32_MIN;
	int32_t min_y = INT32_MAX;
	int32_t max_y = INT32_MIN;
	struct wvnc_output *output;
	wl_list_for_each(output, &wvnc->outputs, link) {
		min_x = min(min_x, output->x);
		max_x = max(max_x, output->x + (int32_t)output->width);
		min_y = min(min_y, output->y);
		max_y = max(max_y, output->y + (int32_t)output->height);
	}
	wvnc->logical_width = max_x - min_x;
	wvnc->logical_height = max_y - min_y;
}


static void handle_keyboard_keymap(void *data, struct wl_keyboard *wl,
								   uint32_t format, int32_t fd, uint32_t size)
{
	struct wvnc *wvnc = data;
	if (wvnc->xkb.map != NULL) {
		return; // We already have a keymap from somewhere
	}

	void *mem = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	if (mem == MAP_FAILED) {
		return;
	}
	wvnc->xkb.map = xkb_keymap_new_from_string(wvnc->xkb.ctx, mem,
		XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
}


static void handle_keyboard_enter(void *data, struct wl_keyboard *wl,
								  uint32_t serial, struct wl_surface *wls,
								  struct wl_array *keys)
{
}


static void handle_keyboard_leave(void *data, struct wl_keyboard *wl,
								  uint32_t serial, struct wl_surface *wls)
{
}


static void handle_keyboard_key(void *data, struct wl_keyboard *wl,
								uint32_t serial, uint32_t time, uint32_t key,
								uint32_t state)
{
}


static void handle_keyboard_modifiers(void *data, struct wl_keyboard *wl,
									  uint32_t serial, uint32_t mods_depressed,
									  uint32_t mods_latched, uint32_t mods_locked,
									  uint32_t group)
{
}


static void handle_keyboard_repeat_info(void *data, struct wl_keyboard *wl,
										int32_t rate, int32_t delay)
{
}


static const struct wl_keyboard_listener wl_keyboard_listener = {
	.keymap = handle_keyboard_keymap,
	.enter = handle_keyboard_enter,
	.leave = handle_keyboard_leave,
	.key = handle_keyboard_key,
	.modifiers = handle_keyboard_modifiers,
	.repeat_info = handle_keyboard_repeat_info,
};


static void init_virtual_keyboard(struct wvnc *wvnc)
{
	// Now we create all the XKB context
	struct wvnc_xkb *xkb = &wvnc->xkb;
	xkb->ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (xkb->ctx == NULL) {
		fail("Failed to create XKB context");
	}

	// We need to get a keymap _somewhere_. This could be either from our
	// selected seat (preferred) or from libxkb (worse).
	if (wvnc->selected_seat->capabilities & WL_SEAT_CAPABILITY_KEYBOARD) {
		struct wl_keyboard *wl_keyboard =
			wl_seat_get_keyboard(wvnc->selected_seat->wl);
		wl_keyboard_add_listener(wl_keyboard, &wl_keyboard_listener, wvnc);
		wl_display_dispatch(wvnc->wl.display);
		wl_display_roundtrip(wvnc->wl.display);
	}
	if (xkb->map == NULL) {
		// Either getting the keymap from wl_seat failed or it has no keyboard
		// attached. So we try to get a generic keymap and hope for the best.
		// TODO: Maybe at least un-hardcode this?
		struct xkb_rule_names names = {
			.rules = "",
			.model = "",
			.layout = "us",
			.variant = "",
			.options = ""
		};
		xkb->map = xkb_keymap_new_from_names(xkb->ctx, &names, 0);
	}
	if (xkb->map == NULL) {
		fail("Failed to load keymap");
	}
	wvnc->wl.keyboard = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(
		wvnc->wl.keyboard_manager, wvnc->selected_seat->wl
	);

	xkb->state = xkb_state_new(xkb->map);
	if (xkb->state == NULL) {
		fail("Failed to create XKB state");
	}

	int fd = shm_create();
	char *str = xkb_keymap_get_as_string(xkb->map, XKB_KEYMAP_USE_ORIGINAL_FORMAT);
	ssize_t length = strlen(str) + 1;
	ssize_t ret = write(fd, str, length);
	if (ret != length) {
		fail("Failed to send keymap to the virtual keyboard");
	}

	zwp_virtual_keyboard_v1_keymap(wvnc->wl.keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, length);
	wl_display_dispatch_pending(wvnc->wl.display);
	log_info("Uploaded keymap to the virtual keyboard");
}


static void init_wayland(struct wvnc *wvnc)
{
	wvnc->wl.display = wl_display_connect(NULL);
	if (wvnc->wl.display == NULL) {
		fail("Failed to connect to the Wayland display");
	}
	wl_list_init(&wvnc->outputs);
	wl_list_init(&wvnc->seats);
	for (size_t i = 0; i < ARRAY_SIZE(wvnc->buffers); i++) {
		wvnc->buffers[i].wvnc = wvnc;
	}
	wvnc->wl.registry = wl_display_get_registry(wvnc->wl.display);
	wl_registry_add_listener(wvnc->wl.registry, &registry_listener, wvnc);
	wl_display_dispatch(wvnc->wl.display);
	wl_display_roundtrip(wvnc->wl.display);
	if (wvnc->wl.screencopy_manager == NULL) {
		fail("wlr-screencopy protocol not supported!");
	}
	if (wvnc->wl.output_manager == NULL) {
		fail("xdg-output-manager protocol not supported");
	}
	// Here we load output info
	struct wvnc_output *output;
	wl_list_for_each(output, &wvnc->outputs, link) {
		output->xdg = zxdg_output_manager_v1_get_xdg_output(
			wvnc->wl.output_manager, output->wl
		);
		zxdg_output_v1_add_listener(output->xdg, &xdg_output_listener, output);
	}
	wl_display_dispatch(wvnc->wl.display);
	wl_display_roundtrip(wvnc->wl.display);
	// Now we select our seat for the virtual keyboard
	struct wvnc_seat *seat;
	wl_list_for_each(seat, &wvnc->seats, link) {
		// TODO: Actual seat selection code
		wvnc->selected_seat = seat;
		break;
	}

	if (wvnc->wl.keyboard_manager != NULL && wvnc->selected_seat != NULL) {
		init_virtual_keyboard(wvnc);
	} else {
		log_error("Unable to initialize the virtual keyboard");
	}
	calculate_logical_size(wvnc);
	log_info("Wayland initialized");

	// Find the correct output to use
	if (wl_list_length(&wvnc->outputs) > 1) {
		if (wvnc->args.output == NULL) {
			fail("Multiple outputs specified but none explicitly selected");
		}
		struct wvnc_output *out;
		wl_list_for_each(out, &wvnc->outputs, link) {
			if (!strcmp(out->name, wvnc->args.output)) {
				wvnc->selected_output = out;
				break;
			}
		}
	} else {
		struct wvnc_output *out;
		wl_list_for_each(out, &wvnc->outputs, link) {
			wvnc->selected_output = out;
			break;
		}
	}
	if (wvnc->selected_output == NULL) {
		fail("No output found");
	}
}


static void init_rfb(struct wvnc *wvnc)
{
	log_info("Initializing RFB");
	// 4 bytes per pixel only at the moment. Probably not worth using anything
	// else.
	wvnc->rfb.screen_info = rfbGetScreen(
		NULL, NULL,
		wvnc->selected_output->width, wvnc->selected_output->height,
		8, 3, 4
	);
	wvnc->rfb.screen_info->desktopName = "wvnc";
	wvnc->rfb.screen_info->alwaysShared = true;
	wvnc->rfb.screen_info->port = wvnc->args.port;
	wvnc->rfb.screen_info->listenInterface = wvnc->args.address;
	// TODO: Maybe enable IPv6 someday
	wvnc->rfb.screen_info->ipv6port = 0;
	wvnc->rfb.screen_info->listen6Interface = NULL;
	wvnc->rfb.screen_info->screenData = wvnc;
	wvnc->rfb.screen_info->newClientHook = rfb_new_client_hook;
	wvnc->rfb.screen_info->kbdAddEvent = rfb_key_hook;
	wvnc->rfb.screen_info->ptrAddEvent = rfb_ptr_hook;

	rfbLog = log_info;
	rfbErr = log_error;

	size_t fb_size = wvnc->selected_output->width * wvnc->selected_output->height * sizeof(rgba_t);
	wvnc->rfb.fb = xmalloc(fb_size);
	wvnc->rfb.screen_info->frameBuffer = (char *)wvnc->rfb.fb;

	log_info("Starting the VNC server");
	rfbInitServer(wvnc->rfb.screen_info);
}


const char *argp_program_version = "wvnc 0.0";
const char *argp_program_bug_address = "<atx@atx.name>";


static struct argp_option argp_options[] = {
	{ "output", 'o', "OUTPUT", 0, "Select output", 0 },
	{ "bind", 'b', "ADDRESS", 0, "Select bind address", 0 },
	{ "port", 'p', "PORT", 0, "Select port", 0 },
	{ "period", 't', "PERIOD", 0, "Sampling period in ms", 0 },
	{ "no-uinput", 'U', NULL, 0, "Disable uinput tablet", 0 },
	{ NULL, 0, NULL, 0, NULL, 0 }
};


static error_t parse_opt(int key, char *arg, struct argp_state *state)
{
	struct wvnc_args *args = state->input;
	switch(key) {
	case 'o':
		args->output = arg;
		break;
	case 'b':
		args->address = inet_addr(arg);
		if (args->address == INADDR_NONE) {
			argp_failure(state, EXIT_FAILURE, 0, "Invalid bind address");
		}
		break;
	case 'p':
		args->port = atoi(arg);
		if (args->port <= 0) {
			argp_failure(state, EXIT_FAILURE, 0, "Invalid port");
		}
		break;
	case 't':
		args->period = atoi(arg);
		if (args->period <= 0) {
			argp_failure(state, EXIT_FAILURE, 0, "Invalid period");
		}
		break;
	case 'U':
		args->no_uinput = true;
		break;
	default:
		return ARGP_ERR_UNKNOWN;
	}
	return 0;
}

void handle_capture_timeout(uv_timer_t *timer)
{
	struct wvnc* wvnc = wl_container_of(timer, wvnc, capture_timer);

	struct zwlr_screencopy_frame_v1 *frame;
	struct wvnc_buffer *buffer, *old;

	switch (wvnc->capture_state) {
	case WVNC_STATE_IDLE:
		wvnc->buffer_i ^= 1;
		buffer = &wvnc->buffers[wvnc->buffer_i];

		frame = zwlr_screencopy_manager_v1_capture_output(
				wvnc->wl.screencopy_manager, 0,
				wvnc->selected_output->wl);

		zwlr_screencopy_frame_v1_add_listener(frame, &frame_listener,
				buffer);

		wvnc->capture_state = WVNC_STATE_CAPTURING;
		break;
	case WVNC_STATE_CAPTURING:
		buffer = &wvnc->buffers[wvnc->buffer_i];
		if (!buffer->done)
			break;

		wvnc->capture_state = WVNC_STATE_IDLE;
		old = &wvnc->buffers[!wvnc->buffer_i];

		if (!wvnc->is_first_capture_done) {
			update_framebuffer_full(wvnc, buffer);
			wvnc->is_first_capture_done = 1;
		} else {
			update_framebuffer_with_damage_check(wvnc, old, buffer);
		}

		rfbProcessEvents(wvnc->rfb.screen_info, 0);
		break;
	}
}

void handle_wayland_event(uv_poll_t *handle, int status, int event)
{
	struct wvnc* wvnc = wl_container_of(handle, wvnc, wayland_poller);
	wl_display_dispatch(wvnc->wl.display);
}

void handle_rfb_event(uv_poll_t *handle, int status, int event)
{
	struct wvnc* wvnc = wl_container_of(handle, wvnc, rfb_poller);
	rfbProcessEvents(wvnc->rfb.screen_info, 0);
}

void clean_up_clients(struct wvnc *wvnc)
{
	struct wvnc_client *client, *tmp;

	wl_list_for_each_safe(client, tmp, &wvnc->clients, link)
		destroy_client(client);
}

void prepare_for_poll(uv_prepare_t *handle)
{
	struct wvnc* wvnc = wl_container_of(handle, wvnc, flusher);
	wl_display_flush(wvnc->wl.display);
}

int main(int argc, char *argv[])
{
	struct wvnc *wvnc = xmalloc(sizeof(struct wvnc));
	global_wvnc = wvnc;
	wvnc->args.port = 5100;
	wvnc->args.address = inet_addr("127.0.0.1");
	wvnc->args.period = 30;  // 30 FPS-ish

	struct argp argp = { argp_options, parse_opt, NULL, NULL, NULL, NULL, NULL };
	argp_parse(&argp, argc, argv, 0, NULL, &wvnc->args);

	// Initialize uinput
	// For some reason, we absolutely have to initialize this
	// before initializing wayland
	if (!wvnc->args.no_uinput) {
		int ret = uinput_init(&wvnc->uinput);
		if (ret) {
			log_error("Failed to initialize uinput: %s", strerror(errno));
		}
	}
	init_wayland(wvnc);
	// TODO: Handle size and transformations
	log_info("Starting on output %s with resolution %dx%d",
			 wvnc->selected_output->name,
			 wvnc->selected_output->width, wvnc->selected_output->height);

	wl_list_init(&wvnc->clients);

	// Initialize RFB
	init_rfb(wvnc);

	uv_loop_init(&wvnc->main_loop);

	uv_timer_init(&wvnc->main_loop, &wvnc->capture_timer);
	uv_timer_start(&wvnc->capture_timer, handle_capture_timeout,
		       wvnc->args.period, wvnc->args.period);

	uv_poll_init(&wvnc->main_loop, &wvnc->wayland_poller,
			wl_display_get_fd(wvnc->wl.display));
	uv_poll_start(&wvnc->wayland_poller, UV_READABLE | UV_PRIORITIZED,
			handle_wayland_event);

	int listen_fd = wvnc->rfb.screen_info->listenSock;
	assert(listen_fd >= 0);
	uv_poll_init(&wvnc->main_loop, &wvnc->rfb_poller, listen_fd);
	uv_poll_start(&wvnc->rfb_poller, UV_READABLE | UV_PRIORITIZED,
			handle_rfb_event);

	uv_prepare_init(&wvnc->main_loop, &wvnc->flusher);
	uv_prepare_start(&wvnc->flusher, prepare_for_poll);

	int rc = uv_run(&wvnc->main_loop, UV_RUN_DEFAULT);

	clean_up_clients(wvnc);

	uv_loop_close(&wvnc->main_loop);

	free(wvnc);

	return rc;
}

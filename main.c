
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <wayland-client.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <rfb/rfb.h>

#include "wayland-xdg-output-client-protocol.h"
#include "wayland-wlr-screencopy-client-protocol.h"

#include "utils.h"


struct wvnc;
struct wvnc_output;

struct rgba {
	uint8_t r;
	uint8_t g;
	uint8_t b;
	uint8_t a;
} __attribute__((packed));
typedef struct rgba rgba_t;
static_assert(sizeof(struct rgba) == 4, "Invalid size of struct rgba");


struct wvnc_buffer {
	struct wvnc *wvnc;

	struct wl_buffer *wl;
	void *data;
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	size_t size;
	enum wl_shm_format format;
	bool y_invert;

	bool done;
};


struct wvnc {
	struct {
		rfbScreenInfo *screen_info;
		rgba_t *fb;
		rgba_t *fb_next;
	} rfb;
	struct {
		struct wl_display *display;
		struct wl_registry *registry;
		struct wl_shm *shm;
		struct zxdg_output_manager_v1 *output_manager;
		struct zwlr_screencopy_manager_v1 *screencopy_manager;
	} wl;

	struct wvnc_buffer buffers[16];

	struct wl_list outputs;
	struct wvnc_output *selected_output;
};


struct wvnc_output {
	struct wl_output *wl;
	struct zxdg_output_v1 *xdg;
	struct wl_list link;

	uint32_t width;
	uint32_t height;
	enum wl_output_transform transform;

	const char *name;
};


static void initialize_shm_buffer(struct wvnc_buffer *buffer,
								  enum wl_shm_format format,
								  uint32_t width, uint32_t height,
								  uint32_t stride)
{
	const char *filename_format = "/wvnc-%d";
	char filename[sizeof(filename_format) + 10];
	int fd = -1;
	for (int i = 0; i < 1000; i++) {
		snprintf(filename, sizeof(filename), filename_format, i);
		fd = shm_open(filename, O_RDWR | O_EXCL | O_CREAT | O_TRUNC, 0660);
		if (fd >= 0) {
			// Just the fd matters now
			shm_unlink(filename);
		}
	}
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
	struct wvnc_output *output = data;
	if (flags & WL_OUTPUT_MODE_CURRENT) {
		output->width = width;
		output->height = height;
	}
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
}


static void handle_xdg_output_logical_size(void *data,
										   struct zxdg_output_v1 *xdg,
										   int32_t width, int32_t height)
{
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


static void handle_wl_registry_global(void *data, struct wl_registry *registry,
									  uint32_t name, const char *interface,
									  uint32_t version)
{
	struct wvnc *wvnc = data;
	if (!strcmp(interface, wl_output_interface.name)) {
		struct wvnc_output *out = xmalloc(sizeof(struct wvnc_output));
		out->wl = wl_registry_bind(registry, name, &wl_output_interface, 1);
		wl_output_add_listener(out->wl, &output_listener, out);
		wl_list_insert(&wvnc->outputs, &out->link);
	} else if (!strcmp(interface, zxdg_output_manager_v1_interface.name)) {
		// TODO: Load names
		wvnc->wl.output_manager = wl_registry_bind(
			registry, name, &zxdg_output_manager_v1_interface, min(version, 2u)
		);
	} else if (!strcmp(interface, zwlr_screencopy_manager_v1_interface.name)) {
		wvnc->wl.screencopy_manager = wl_registry_bind(registry, name,
				&zwlr_screencopy_manager_v1_interface, 1);
	} else if (!strcmp(interface, wl_shm_interface.name)) {
		wvnc->wl.shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	}
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


static void init_wayland(struct wvnc *wvnc)
{
	wvnc->wl.display = wl_display_connect(NULL);
	if (wvnc->wl.display == NULL) {
		fail("Failed to connect to the Wayland display");
	}
	wl_list_init(&wvnc->outputs);
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
	struct wvnc_output *output;
	wl_list_for_each(output, &wvnc->outputs, link) {
		output->xdg = zxdg_output_manager_v1_get_xdg_output(
			wvnc->wl.output_manager, output->wl
		);
		zxdg_output_v1_add_listener(output->xdg, &xdg_output_listener, output);
	}
	wl_display_dispatch(wvnc->wl.display);
	wl_display_roundtrip(wvnc->wl.display);
	log_info("Wayland initialized with %d outputs", wl_list_length(&wvnc->outputs));
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
	// TODO: Command line arguments here
	wvnc->rfb.screen_info->desktopName = "wvnc";
	wvnc->rfb.screen_info->alwaysShared = true;
	wvnc->rfb.screen_info->port = 5100;
	// TODO: Set rfbLog/rfbErr

	size_t fb_size = wvnc->selected_output->width * wvnc->selected_output->height * sizeof(rgba_t);
	wvnc->rfb.fb = xmalloc(fb_size);
	wvnc->rfb.fb_next = xmalloc(fb_size);
	wvnc->rfb.screen_info->frameBuffer = (char *)wvnc->rfb.fb;

	log_info("Starting the VNC server");
	rfbInitServer(wvnc->rfb.screen_info);
}


static void copy_to_next_fb(struct wvnc *wvnc, struct wvnc_buffer *buffer)
{
	// TODO: Support different formats
	// TODO: Support transforms
	for (uint32_t y = 0; y < buffer->height; y++) {
		for (uint32_t x = 0; x < buffer->width; x++) {
			assert(buffer->format == WL_SHM_FORMAT_ARGB8888);
			uint32_t src = *(uint32_t *)(buffer->data + y * buffer->stride + x * 4);
			rgba_t c = {
				.r = (src >> 16) & 0xff,
				.g = (src >>  8) & 0xff,
				.b = (src >>  0) & 0xff,
				.a = 0xff,
			};
			rgba_t *tgt = &wvnc->rfb.fb_next[y*wvnc->selected_output->width + x];
			*tgt = c;
		}
	}
}


static void update_framebuffer(struct wvnc *wvnc)
{
	// TODO: Maybe get something more sophisticated here?
	uint64_t scanline_damage_map[wvnc->selected_output->height / 64 + 1];
	memset(scanline_damage_map, 0, sizeof(scanline_damage_map));

	for (uint32_t y = 0; y < wvnc->selected_output->height; y++) {
		bool damaged = false;
		for (uint32_t x = 0; x < wvnc->selected_output->width; x++) {
			size_t off = y*wvnc->selected_output->width + x;
			rgba_t src = wvnc->rfb.fb_next[off];
			rgba_t *tgt = &wvnc->rfb.fb[off];
			if (src.r != tgt->r || src.g != tgt->g || src.b != tgt->b || src.a != tgt->a) {
				*tgt = src;
				damaged = true;
			}
		}
		if (damaged) {
			scanline_damage_map[y / 64] |= 1 << (y % 64);
		}
	}

	for (uint32_t y = 0; y < wvnc->selected_output->height; y++) {
		uint64_t bits = scanline_damage_map[y / 64];
		if (bits & (1 << (y & 64))) {
			rfbMarkRectAsModified(
					wvnc->rfb.screen_info,
					0, y, wvnc->selected_output->width, y + 1
			);
		}
	}
}


int main(int argc, const char *argv[])
{
	struct wvnc *wvnc = xmalloc(sizeof(struct wvnc));

	init_wayland(wvnc);

	const char *output_name = "DP-1";
	struct wvnc_output *out;
	wl_list_for_each(out, &wvnc->outputs, link) {
		if (!strcmp(out->name, output_name)) {
			wvnc->selected_output = out;
			break;
		}
	}
	if (wvnc->selected_output == NULL) {
		fail("Output %s not found", output_name);
	}
	// TODO: Handle size and transformations
	log_info("Starting on output %s with resolution %dx%d",
			 wvnc->selected_output->name,
			 wvnc->selected_output->width, wvnc->selected_output->height);

	// Initialize RFB
	init_rfb(wvnc);

	// Start capture
	uint64_t last_capture = 0; // Start of last capture
	const uint64_t capture_period = 16 * 1000;  // 30 FPS-ish
	struct zwlr_screencopy_frame_v1 *frame = NULL;
	bool capturing = false;
	while (true) {
		struct wvnc_buffer *buffer = &wvnc->buffers[0];
		// TODO: Should we composite the cursor or not?
		uint64_t t_now = time_monotonic();
		uint64_t t_delta = t_now - last_capture;
		if (t_delta >= capture_period && !capturing) {
			last_capture = t_now;
			capturing = true;
			buffer->done = false;
			frame = zwlr_screencopy_manager_v1_capture_output(
				wvnc->wl.screencopy_manager, 0, wvnc->selected_output->wl
			);
			zwlr_screencopy_frame_v1_add_listener(frame, &frame_listener, buffer);
			wl_display_dispatch(wvnc->wl.display);
			wl_display_flush(wvnc->wl.display);
		} else if (capturing && buffer->done) {
			capturing = false;
			assert(wvnc->selected_output->width == buffer->width &&
				   wvnc->selected_output->height == buffer->height);

			copy_to_next_fb(wvnc, buffer);
			update_framebuffer(wvnc);

			zwlr_screencopy_frame_v1_destroy(frame);
		}

		// TODO: Maybe use epoll or something
		struct timeval timeout = {
			.tv_sec = 0,
			.tv_usec = capture_period
		};
		int wl_fd = wl_display_get_fd(wvnc->wl.display);
		fd_set fds;
		memcpy(&fds, &wvnc->rfb.screen_info->allFds, sizeof(fd_set));
		FD_SET(wl_fd, &fds);
		int ret = select(wvnc->rfb.screen_info->maxFd + 2, &fds, NULL, NULL, &timeout);
		if (ret != 0) {
			bool is_wl = FD_ISSET(wl_fd, &fds);
			if (is_wl) {
				wl_display_dispatch(wvnc->wl.display);
			}
			// No way of checking directly
			if ((is_wl && ret > 1) || ret == 1) {
				rfbProcessEvents(wvnc->rfb.screen_info, 0);
			}
		}
	}


	free(wvnc);
}

#pragma once

#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <wayland-client.h>

#include "uinput.h"


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
	uint32_t stride;

	struct nvnc_fb fb;

	bool done;
};


struct wvnc_output {
	struct wl_output *wl;
	struct zxdg_output_v1 *xdg;
	struct wl_list link;

	int32_t x;
	int32_t y;
	uint32_t width;
	uint32_t height;
	enum wl_output_transform transform;

	uint32_t fourcc;

	char *name;
};


struct wvnc_seat {
	struct wl_seat *wl;
	char *name;
	uint32_t capabilities;

	struct wl_list link;
};

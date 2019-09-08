/*
 * Copyright (c) 2019 Andri Yngvason
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <pixman.h>

#include "bitmap.h"
#include "damage.h"

#define TILE_SIZE 64

/* This function has been optimised for auto-vectorization */
struct bitmap *damage_compute(const uint32_t * __restrict__ src0,
							  const uint32_t * __restrict__ src1,
							  int width, int height)
{
	int x_tiles = UDIV_UP(width, TILE_SIZE);
	int y_tiles = UDIV_UP(height, TILE_SIZE);

	struct bitmap *damage = bitmap_alloc(x_tiles * y_tiles);
	if (!damage)
		return NULL;

	int partial_width = (width / TILE_SIZE) * TILE_SIZE;
	int residual_width = width - partial_width;

	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < partial_width; x += TILE_SIZE) {
			int buffer_index = y * width + x;
			int tile_index = y / TILE_SIZE * x_tiles + x / TILE_SIZE;

			if (bitmap_is_set(damage, tile_index))
				continue;

			int is_tile_damaged = 0;

			/* This loop should be auto-vectorized */
			for (int i = buffer_index; i < buffer_index + TILE_SIZE; ++i) {
				is_tile_damaged |= src0[i] != src1[i];
			}

			bitmap_set_cond(damage, tile_index, is_tile_damaged);
		}

		int buffer_index = y * width + partial_width;
		int tile_index = y / TILE_SIZE * x_tiles
			       + partial_width / TILE_SIZE;

		if (bitmap_is_set(damage, tile_index))
			continue;

		int is_tile_damaged = 0;

		/* This loop should be auto-vectorized */
		for (int i = buffer_index; i < buffer_index + residual_width; ++i) {
			is_tile_damaged |= src0[i] != src1[i];
		}

		bitmap_set_cond(damage, tile_index, is_tile_damaged);
	}

	return damage;
}

void damage_to_pixman(struct pixman_region16* dst, const struct bitmap* src,
					  int width, int height)
{
	int x_tiles = UDIV_UP(width, TILE_SIZE);
	int y_tiles = UDIV_UP(height, TILE_SIZE);

	for (int yt = 0; yt < y_tiles; ++yt)
		for (int xt = 0; xt < x_tiles; ++xt)
			if (bitmap_is_set(src, yt * x_tiles + xt)) {
				int x = xt * TILE_SIZE;
				int y = (y_tiles - yt - 1) * TILE_SIZE;

				pixman_region_union_rect(dst, dst, x, y,
							 TILE_SIZE, TILE_SIZE);
			}

	pixman_region_intersect_rect(dst, dst, 0, 0, width, height);
}

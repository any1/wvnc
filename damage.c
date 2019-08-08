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

struct bitmap *damage_compute(const uint32_t *src0, const uint32_t *src1,
			      int width, int height)
{
	struct bitmap *damage = NULL, *row_damage = NULL;

	int x_tiles = UDIV_UP(width, 64);
	int y_tiles = UDIV_UP(height, 64);

	row_damage = bitmap_alloc(width);
	if (!row_damage)
		goto failure;

	damage = bitmap_alloc(x_tiles * y_tiles);
	if (!damage)
		goto failure;

	for (int y = 0; y < height; ++y) {
		bitmap_clear_all(row_damage);
		get_row_damage(row_damage,
			       src0 + y * width,
			       src1 + y * width,
			       width);
		damage_reduce_by_64(damage, y / 64 * x_tiles, row_damage);
	}

	free(row_damage);
	return damage;

failure:
	free(damage);
	free(row_damage);
	return NULL;
}

void damage_to_pixman(struct pixman_region32* dst, const struct bitmap* src,
		      int width, int height)
{
	int x_tiles = UDIV_UP(width, 64);
	int y_tiles = UDIV_UP(height, 64);

	for (int i = 0; i < x_tiles * y_tiles; ++i)
		if (bitmap_is_set(src, i)) {
			int x = (i % x_tiles) * 64;
			int y = (i / x_tiles) * 64;

			pixman_region32_union_rect(dst, dst, x, y, 64, 64);
		}

	pixman_region32_intersect_rect(dst, dst, 0, 0, width, height);
}

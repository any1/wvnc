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

#pragma once

#include <stdint.h>
#include <unistd.h>

#include "bitmap.h"

static inline void get_row_damage(struct bitmap* dst, const uint32_t *row0,
		const uint32_t *row1, size_t src_len)
{
	for (size_t i = 0; i < src_len; ++i)
		bitmap_set_cond(dst, i, row0[i] != row1[i]);
}

static inline void damage_reduce_by_64(struct bitmap *dst, size_t shift,
				       const struct bitmap *src)
{
	for (size_t i = 0; i < src->n_elem; ++i)
		bitmap_set_cond(dst, i + shift, !!src->data[i]);
}

struct bitmap *damage_compute(const uint32_t *src0, const uint32_t *src1,
			      int width, int height);

void damage_to_pixman(struct pixman_region32* dst, const struct bitmap* src,
		      int width, int height);

/*  
 * Copyright (c) 2014, Citrix Systems Inc.
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 
 * Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 * 
 * Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 * TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

/*
 * Copyright (c) 2003 Fabrice Bellard
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
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <err.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <fcntl.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "debug.h"
#include "demu.h"
#include "vga.h"
#include "surface.h"

#define FALSE 0
#define TRUE  1

#define GMODE_TEXT      0
#define GMODE_GRAPHIC   1
#define GMODE_BLANK     2

#define CH_ATTR_SIZE (160 * 100)
#define VGA_MAX_HEIGHT 2048

typedef struct surface {
    uint8_t             *framebuffer;
    uint32_t            font_offsets[2];
    int                 graphic_mode;
    uint8_t             shift_control;
    uint8_t             double_scan;
    uint32_t            line_offset;
    uint32_t            line_compare;
    uint32_t            start_addr;
    uint32_t            linesize;
    uint32_t            last_line_offset;
    uint8_t             last_cw;
    uint8_t             last_ch;
    uint32_t            last_width;
    uint32_t            last_height;
    uint32_t            last_scr_width;
    uint32_t            last_scr_height;
    uint32_t            last_depth;
    uint8_t             cursor_start;
    uint8_t             cursor_end;
    uint32_t            cursor_offset;
    unsigned int        (*rgb_to_pixel)(unsigned int r, unsigned int g, unsigned b);
    uint32_t            last_palette[256];
    uint32_t            last_ch_attr[CH_ATTR_SIZE];
    uint32_t            invalidated_y_table[VGA_MAX_HEIGHT / 32];
} surface_t;

static surface_t    surface_state;

static uint32_t     expand4[256];
static uint16_t     expand2[256];
static uint8_t      expand4to8[16];

static void get_offsets(surface_t *s,
                        uint32_t *pline_offset,
                        uint32_t *pstart_addr,
                        uint32_t *pline_compare)
{
    uint32_t start_addr, line_offset, line_compare;

    if (vga_get_vbe_regs(VBE_DISPI_INDEX_ENABLE) & VBE_DISPI_ENABLED) {
        line_offset = vga_get_vbe_line_offset();
        start_addr = vga_get_vbe_start_addr();
        line_compare = 65535;
    } else {
        /* compute line_offset in bytes */
        line_offset = vga_get_cr(0x13);
        line_offset <<= 3;

        /* starting address */
        start_addr = vga_get_cr(0x0d) | (vga_get_cr(0x0c) << 8);

        /* line compare */
        line_compare = vga_get_cr(0x18) |
            ((vga_get_cr(0x07) & 0x10) << 4) |
            ((vga_get_cr(0x09) & 0x40) << 3);
    }
    *pline_offset = line_offset;
    *pstart_addr = start_addr;
    *pline_compare = line_compare;
}

static int get_bpp(surface_t *s)
{
    int ret;

    if (vga_get_vbe_regs(VBE_DISPI_INDEX_ENABLE) & VBE_DISPI_ENABLED) {
        ret = vga_get_vbe_regs(VBE_DISPI_INDEX_BPP);
    } else  {
        ret = 0;
    }

    return ret;
}

static void get_resolution(surface_t *s, int *pwidth, int *pheight)
{
    int width, height;

    if (vga_get_vbe_regs(VBE_DISPI_INDEX_ENABLE) & VBE_DISPI_ENABLED) {
        width = vga_get_vbe_regs(VBE_DISPI_INDEX_XRES);
        height = vga_get_vbe_regs(VBE_DISPI_INDEX_YRES);
    } else  {
        width = (vga_get_cr(0x01) + 1) * 8;
        height = vga_get_cr(0x12) |
            ((vga_get_cr(0x07) & 0x02) << 7) |
            ((vga_get_cr(0x07) & 0x40) << 3);
        height = (height + 1);
    }
    *pwidth = width;
    *pheight = height;
}

static inline int c6_to_8(int v)
{
    int b;
    v &= 0x3f;
    b = v & 1;
    return (v << 2) | (b << 1) | b;
}

/* return true if the palette was modified */
static int vgpu_update_palette16(surface_t *s)
{
    int full_update, i;
    uint32_t v, col, *palette;

    full_update = 0;
    palette = s->last_palette;
    for(i = 0; i < 16; i++) {
        v = vga_get_ar(i);
        if (vga_get_ar(0x10) & 0x80)
            v = ((vga_get_ar(0x14) & 0xf) << 4) | (v & 0xf);
        else
            v = ((vga_get_ar(0x14) & 0xc) << 4) | (v & 0x3f);
        v = v * 3;
        col = s->rgb_to_pixel(c6_to_8(vga_get_palette(v)),
                              c6_to_8(vga_get_palette(v + 1)),
                              c6_to_8(vga_get_palette(v + 2)));
        if (col != palette[i]) {
            full_update = 1;
            palette[i] = col;
        }
    }
    return full_update;
}

static int vgpu_update_palette256(surface_t *s)
{
    int full_update, i;
    uint32_t v, col, *palette;

    full_update = 0;
    palette = s->last_palette;
    v = 0;
    for(i = 0; i < 256; i++) {
        if (vga_is_dac_8bit()) {
          col = s->rgb_to_pixel(vga_get_palette(v),
                                vga_get_palette(v + 1),
                                vga_get_palette(v + 2));
        } else {
          col = s->rgb_to_pixel(c6_to_8(vga_get_palette(v)),
                                c6_to_8(vga_get_palette(v + 1)),
                                c6_to_8(vga_get_palette(v + 2)));
        }
        if (col != palette[i]) {
            full_update = 1;
            palette[i] = col;
        }
        v += 3;
    }
    return full_update;
}

/* update start_addr and line_offset. Return TRUE if modified */
static int update_basic_params(surface_t *s)
{
    int full_update;
    uint32_t start_addr, line_offset, line_compare;

    full_update = 0;

    get_offsets(s, &line_offset, &start_addr, &line_compare);

    if (line_offset != s->line_offset ||
        start_addr != s->start_addr ||
        line_compare != s->line_compare) {
        s->line_offset = line_offset;
        s->start_addr = start_addr;
        s->line_compare = line_compare;
        full_update = 1;
    }
    return full_update;
}

static inline unsigned int rgb_to_pixel32(unsigned int r, unsigned int g, unsigned b)
{
    return (b << 16) | (g << 8) | r;
}

static unsigned int rgb_to_pixel32_dup(unsigned int r, unsigned int g, unsigned b)
{
    unsigned int col;
    col = rgb_to_pixel32(r, g, b);
    return col;
}

#define PAT(x) (x)

static const uint32_t mask16[16] = {
    PAT(0x00000000),
    PAT(0x000000ff),
    PAT(0x0000ff00),
    PAT(0x0000ffff),
    PAT(0x00ff0000),
    PAT(0x00ff00ff),
    PAT(0x00ffff00),
    PAT(0x00ffffff),
    PAT(0xff000000),
    PAT(0xff0000ff),
    PAT(0xff00ff00),
    PAT(0xff00ffff),
    PAT(0xffff0000),
    PAT(0xffff00ff),
    PAT(0xffffff00),
    PAT(0xffffffff),
};

#undef PAT

#define cbswap_32(__x) \
((uint32_t)( \
		(((uint32_t)(__x) & (uint32_t)0x000000ffUL) << 24) | \
		(((uint32_t)(__x) & (uint32_t)0x0000ff00UL) <<  8) | \
		(((uint32_t)(__x) & (uint32_t)0x00ff0000UL) >>  8) | \
		(((uint32_t)(__x) & (uint32_t)0xff000000UL) >> 24) ))

#define PAT(x) cbswap_32(x)

static const uint32_t dmask16[16] = {
    PAT(0x00000000),
    PAT(0x000000ff),
    PAT(0x0000ff00),
    PAT(0x0000ffff),
    PAT(0x00ff0000),
    PAT(0x00ff00ff),
    PAT(0x00ffff00),
    PAT(0x00ffffff),
    PAT(0xff000000),
    PAT(0xff0000ff),
    PAT(0xff00ff00),
    PAT(0xff00ffff),
    PAT(0xffff0000),
    PAT(0xffff00ff),
    PAT(0xffffff00),
    PAT(0xffffffff),
};

static const uint32_t dmask4[4] = {
    PAT(0x00000000),
    PAT(0x0000ffff),
    PAT(0xffff0000),
    PAT(0xffffffff),
};

#undef PAT

#define BIG 0

#define GET_PLANE(data, p) (((data) >> ((p) * 8)) & 0xff)

#define xglue(_x, _y) _x##_y
#define glue(_x, _y) xglue(_x, _y)

#define cpu_to_32wu(_p, _v) \
    *(_p) = (_v)

#define lduw_raw(_p) \
    (*(uint16_t *)(_p))

#define DEPTH 32
#include "template.h"

typedef void vga_draw_glyph8_func(uint8_t *d, int linesize,
                             const uint8_t *font_ptr, int h,
                             uint32_t fgcol, uint32_t bgcol);
typedef void vga_draw_glyph16_func(uint8_t *d, int linesize,
                             const uint8_t *font_ptr, int h,
                             uint32_t fgcol, uint32_t bgcol);
typedef void vga_draw_glyph9_func(uint8_t *d, int linesize,
                                  const uint8_t *font_ptr, int h,
                                  uint32_t fgcol, uint32_t bgcol, int dup9);

static vga_draw_glyph8_func *vga_draw_glyph8 = __vga_draw_glyph8_32;
static vga_draw_glyph16_func *vga_draw_glyph16 = __vga_draw_glyph16_32;
static vga_draw_glyph9_func *vga_draw_glyph9 = __vga_draw_glyph9_32;

static const uint8_t cursor_glyph[32 * 4] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};

enum {
    VGA_DRAW_LINE2,
    VGA_DRAW_LINE2D2,
    VGA_DRAW_LINE4,
    VGA_DRAW_LINE4D2,
    VGA_DRAW_LINE8D2,
    VGA_DRAW_LINE8,
    VGA_DRAW_LINE15,
    VGA_DRAW_LINE16,
    VGA_DRAW_LINE24,
    VGA_DRAW_LINE32,
    VGA_DRAW_LINE_NB,
};

typedef void vga_draw_line_func(uint32_t *palette, uint32_t plane_enable, uint8_t *d,
                                const uint8_t *s, int width);

static vga_draw_line_func *vga_draw_line_table[VGA_DRAW_LINE_NB] = {
    __vga_draw_line2_32,
    __vga_draw_line2d2_32,
    __vga_draw_line4_32,
    __vga_draw_line4d2_32,
    __vga_draw_line8d2_32,
    __vga_draw_line8_32,
    __vga_draw_line15_32,
    __vga_draw_line16_32,
    __vga_draw_line24_32,
    __vga_draw_line32_32,
};

typedef unsigned int rgb_to_pixel_dup_func(unsigned int r, unsigned int g, unsigned b);

static rgb_to_pixel_dup_func *rgb_to_pixel_dup = rgb_to_pixel32_dup;

static void
surface_resize(uint32_t width, uint32_t height)
{
    DBG("%ux%u\n", width, height);

    surface_state.linesize = width * 4;

    surface_state.last_scr_width = width;
    surface_state.last_scr_height = height;

    demu_new_framebuffer(width, height, 4);

    surface_state.framebuffer = demu_get_framebuffer();
}

int
surface_initialize(void)
{
    int i;
    int j;
    int v;
    int b;

    for (i = 0; i < 256; i++) {
        v = 0;
        for (j = 0; j < 8; j++)
            v |= ((i >> j) & 1) << (j * 4);
        expand4[i] = v;

        v = 0;
        for (j = 0; j < 4; j++)
            v |= ((i >> (2 * j)) & 3) << (j * 4);
        expand2[i] = v;
    }

    for (i = 0; i < 16; i++) {
        v = 0;
        for (j = 0; j < 4; j++) {
            b = ((i >> j) & 1);
            v |= b << (2 * j);
            v |= b << (2 * j + 1);
        }
        expand4to8[i] = v;
    }

    surface_state.graphic_mode = -1;

    return 0;
}

static void
surface_update(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    demu_update_framebuffer(x, y, width, height);
}

static void
surface_draw_text(surface_t *s, uint8_t *vram, int full_update)
{
    int cx, cy, cheight, cw, ch, cattr, height, width, ch_attr;
    int cx_min, cx_max, linesize, x_incr;
    uint32_t offset, fgcol, bgcol, v, cursor_offset;
    uint8_t *d1, *d, *src, *s1, *dest, *cursor_ptr;
    const uint8_t *font_ptr, *font_base[2];
    int dup9, line_offset;
    uint32_t *palette;
    uint32_t *ch_attr_ptr;
    vga_draw_glyph8_func *__vga_draw_glyph8;
    vga_draw_glyph9_func *__vga_draw_glyph9;

    assert(vram != NULL);

    vga_get_vram_dirty_map(FALSE);

    /* compute font data address (in plane 2) */
    v = vga_get_sr(3);
    offset = (((v >> 4) & 1) | ((v << 1) & 6)) * 8192 * 4 + 2;
    if (offset != s->font_offsets[0]) {
        s->font_offsets[0] = offset;
        full_update = 1;
    }
    font_base[0] = vram + offset;

    offset = (((v >> 5) & 1) | ((v >> 1) & 6)) * 8192 * 4 + 2;
    font_base[1] = vram + offset;
    if (offset != s->font_offsets[1]) {
        s->font_offsets[1] = offset;
        full_update = 1;
    }

    if (vga_test_and_clear_plane2()) {
        /* if the plane 2 was modified since the last display, it
           indicates the font may have been modified */
        full_update = 1;
    }

    full_update |= update_basic_params(s);

    line_offset = s->line_offset;
    s1 = vram + (s->start_addr * 4);

    /* total width & height */
    cheight = (vga_get_cr(9) & 0x1f) + 1;
    cw = 8;
    if (!(vga_get_sr(1) & 0x01))
        cw = 9;
    if (vga_get_sr(1) & 0x08)
        cw = 16; /* NOTE: no 18 pixel wide */
    width = (vga_get_cr(0x01) + 1);
    if (vga_get_cr(0x06) == 100) {
        /* ugly hack for CGA 160x100x16 - explain me the logic */
        height = 100;
    } else {
        height = vga_get_cr(0x12) |
            ((vga_get_cr(0x07) & 0x02) << 7) |
            ((vga_get_cr(0x07) & 0x40) << 3);
        height = (height + 1) / cheight;
    }
    if ((height * width) > CH_ATTR_SIZE) {
        /* better than nothing: exit if transient size is too big */
        return;
    }

    if (width * cw != s->last_scr_width ||
        height * cheight != s->last_scr_height ||
        cw != s->last_cw ||
        cheight != s->last_ch ||
        s->last_depth) {
        surface_resize(width * cw, height * cheight);
        s->last_depth = 0;
        s->last_width = width;
        s->last_height = height;
        s->last_ch = cheight;
        s->last_cw = cw;
        full_update = 1;
    }

    s->rgb_to_pixel = rgb_to_pixel_dup;
    full_update |= vgpu_update_palette16(s);
    palette = s->last_palette;
    x_incr = cw * 4;
    
    cursor_offset = ((vga_get_cr(0x0e) << 8) | vga_get_cr(0x0f)) - s->start_addr;
    if (cursor_offset != s->cursor_offset ||
        vga_get_cr(0xa) != s->cursor_start ||
        vga_get_cr(0xb) != s->cursor_end) {
      /* if the cursor position changed, we update the old and new
         chars */
        if (s->cursor_offset < CH_ATTR_SIZE)
            s->last_ch_attr[s->cursor_offset] = -1;
        if (cursor_offset < CH_ATTR_SIZE)
            s->last_ch_attr[cursor_offset] = -1;
        s->cursor_offset = cursor_offset;
        s->cursor_start = vga_get_cr(0xa);
        s->cursor_end = vga_get_cr(0xb);
    }
    cursor_ptr = vram + (s->start_addr + cursor_offset) * 4;

    if (cw == 16)
        __vga_draw_glyph8 = vga_draw_glyph16;
    else
        __vga_draw_glyph8 = vga_draw_glyph8;
    __vga_draw_glyph9 = vga_draw_glyph9;

    dest = s->framebuffer;
    linesize = s->linesize;
    ch_attr_ptr = s->last_ch_attr;
    for(cy = 0; cy < height; cy++) {
        d1 = dest;
        src = s1;
        cx_min = width;
        cx_max = -1;
        for(cx = 0; cx < width; cx++) {
            ch_attr = *(uint16_t *)src;
            if (full_update || ch_attr != *ch_attr_ptr) {
                if (cx < cx_min)
                    cx_min = cx;
                if (cx > cx_max)
                    cx_max = cx;
                *ch_attr_ptr = ch_attr;
                ch = ch_attr & 0xff;
                cattr = ch_attr >> 8;
                font_ptr = font_base[(cattr >> 3) & 1];
                font_ptr += 32 * 4 * ch;
                bgcol = palette[cattr >> 4];
                fgcol = palette[cattr & 0x0f];
                if (cw != 9) {
                    __vga_draw_glyph8(d1, linesize, font_ptr, cheight, fgcol, bgcol);
                } else {
                    dup9 = 0;
                    if (ch >= 0xb0 && ch <= 0xdf && (vga_get_ar(0x10) & 0x04))
                        dup9 = 1;
                    __vga_draw_glyph9(d1, linesize, font_ptr, cheight, fgcol, bgcol, dup9);
                }
                if (src == cursor_ptr &&
                    !(vga_get_cr(0x0a) & 0x20)) {
                    int line_start, line_last, h;
                    /* draw the cursor */
                    line_start = vga_get_cr(0x0a) & 0x1f;
                    line_last = vga_get_cr(0x0b) & 0x1f;
                    /* XXX: check that */
                    if (line_last > cheight - 1)
                        line_last = cheight - 1;
                    if (line_last >= line_start && line_start < cheight) {
                        h = line_last - line_start + 1;
                        d = d1 + linesize * line_start;
                        if (cw != 9) {
                            __vga_draw_glyph8(d, linesize, cursor_glyph, h, fgcol, bgcol);
                        } else {
                            __vga_draw_glyph9(d, linesize, cursor_glyph, h, fgcol, bgcol, 1);
                        }
                    }
                }
            }
            d1 += x_incr;
            src += 4;
            ch_attr_ptr++;
            if (cx_max != -1)
                surface_update(cx_min * cw, cy * cheight,
                               (cx_max - cx_min + 1) * cw, cheight);

        }
        dest += linesize * cheight;
        s1 += line_offset;
    }
}

static void
surface_draw_graphic(surface_t *s, uint8_t *vram, int full_update)
{
    int y1, y, update, linesize, y_start, double_scan, mask, depth;
    int width, height, shift_control, line_offset, bwidth, bits;
    int disp_width, multi_scan, multi_run;
    uint8_t *d;
    uint32_t v, addr1, addr;
    vga_draw_line_func *__vga_draw_line;

    assert(vram != NULL);

    full_update |= update_basic_params(s);

    get_resolution(s, &width, &height);
    disp_width = width;

    shift_control = (vga_get_gr(0x05) >> 5) & 3;
    double_scan = (vga_get_cr(0x09) >> 7);
    if (shift_control != 1) {
        multi_scan = (((vga_get_cr(0x09) & 0x1f) + 1) << double_scan) - 1;
    } else {
        /* in CGA modes, multi_scan is ignored */
        multi_scan = double_scan;
    }
    multi_run = multi_scan;
    if (shift_control != s->shift_control ||
        double_scan != s->double_scan) {
        full_update = 1;
        s->shift_control = shift_control;
        s->double_scan = double_scan;
    }
    if (shift_control == 1 && (vga_get_sr(0x01) & 8)) {
        disp_width <<= 1;
    }

    if (shift_control == 0) {
        if (vga_get_sr(0x01) & 8) {
            disp_width <<= 1;
        }
    } else if (shift_control == 1) {
        if (vga_get_sr(0x01) & 8) {
            disp_width <<= 1;
        }
    }

    depth = get_bpp(s);
    if (s->line_offset != s->last_line_offset || 
        disp_width != s->last_scr_width ||
        height != s->last_scr_height ||
        s->last_depth != depth) {

        DBG("depth = %d\n", depth);
        surface_resize(disp_width, height);

        s->last_width = disp_width;
        s->last_height = height;
        s->last_line_offset = s->line_offset;
        s->last_depth = depth;
        full_update = 1;
    }

    s->rgb_to_pixel = rgb_to_pixel_dup;

    if (shift_control == 0) {
        full_update |= vgpu_update_palette16(s);
        if (vga_get_sr(0x01) & 8) {
            v = VGA_DRAW_LINE4D2;
        } else {
            v = VGA_DRAW_LINE4;
        }
        bits = 4;
    } else if (shift_control == 1) {
        full_update |= vgpu_update_palette16(s);
        if (vga_get_sr(0x01) & 8) {
            v = VGA_DRAW_LINE2D2;
        } else {
            v = VGA_DRAW_LINE2;
        }
        bits = 4;
    } else {
        switch(get_bpp(s)) {
        default:
        case 0:
            full_update |= vgpu_update_palette256(s);
            v = VGA_DRAW_LINE8D2;
            bits = 4;
            break;
        case 8:
            full_update |= vgpu_update_palette256(s);
            v = VGA_DRAW_LINE8;
            bits = 8;
            break;
        case 15:
            v = VGA_DRAW_LINE15;
            bits = 16;
            break;
        case 16:
            v = VGA_DRAW_LINE16;
            bits = 16;
            break;
        case 24:
            v = VGA_DRAW_LINE24;
            bits = 24;
            break;
        case 32:
            v = VGA_DRAW_LINE32;
            bits = 32;
            break;
        }
    }

    __vga_draw_line = vga_draw_line_table[v];

    line_offset = s->line_offset;

    addr1 = (s->start_addr * 4);
    bwidth = (width * bits + 7) / 8;
    y_start = -1;
    d = s->framebuffer;
    linesize = s->linesize;
    y1 = 0;

    vga_get_vram_dirty_map(TRUE);

    for(y = 0; y < height; y++) {
        addr = addr1;
        if (!(vga_get_cr(0x17) & 1)) {
            int shift;
            /* CGA compatibility handling */
            shift = 14 + ((vga_get_cr(0x17) >> 6) & 1);
            addr = (addr & ~(1 << shift)) | ((y1 & 1) << shift);
        }
        if (!(vga_get_cr(0x17) & 2)) {
            addr = (addr & ~0x8000) | ((y1 & 2) << 14);
        }
        update = full_update | vga_vram_is_dirty(addr, bwidth);
        /* explicit invalidation for the hardware cursor */
        update |= (s->invalidated_y_table[y >> 5] >> (y & 0x1f)) & 1;
        if (update) {
            uint32_t plane_enable;

            if (y_start < 0)
                y_start = y;

            plane_enable = vga_get_ar(0x12) & 0xf;
            __vga_draw_line(s->last_palette, plane_enable, d, vram + addr, width);
        } else {
            if (y_start >= 0) {
                /* flush to display */
                surface_update(0, y_start,
                               disp_width, y - y_start);
                y_start = -1;
            }
        }
        if (!multi_run) {
            mask = (vga_get_cr(0x17) & 3) ^ 3;
            if ((y1 & mask) == mask)
                addr1 += line_offset;
            y1++;
            multi_run = multi_scan;
        } else {
            multi_run--;
        }
        /* line compare acts on the displayed lines */
        if (y == s->line_compare)
            addr1 = 0;
        d += linesize;
    }
    if (y_start >= 0) {
        /* flush to display */
        surface_update(0, y_start,
                       disp_width, y - y_start);
        y_start = -1;
    }

    memset(s->invalidated_y_table, 0, ((height + 31) >> 5) * 4);
}

static void
surface_draw_blank(surface_t *s, int full_update)
{
    int val;

    vga_get_vram_dirty_map(FALSE);

    if (!full_update)
        return;

    if (s->last_scr_width <= 0 || s->last_scr_height <= 0)
        return;

    s->rgb_to_pixel = rgb_to_pixel_dup;
    val = s->rgb_to_pixel(0, 0, 0);

    memset(s->framebuffer, val,
           s->last_scr_width * s->last_scr_height * 4);

    surface_update(0, 0,
                   s->last_scr_width, s->last_scr_height);
}

void
surface_refresh(void)
{
    surface_t *s = &surface_state;
    int full_update;
    int graphic_mode;
    uint8_t *vram;

    vram = vga_get_vram();

    if (!(vga_get_ar_index() & 0x20) || vram == NULL) {
        graphic_mode = GMODE_BLANK;
    } else {
        graphic_mode = vga_get_gr(6) & 1;
    }

    if (graphic_mode != s->graphic_mode) {
        s->graphic_mode = graphic_mode;

        switch(s->graphic_mode) {
        case GMODE_TEXT:
            DBG("text\n");
            break;
        case GMODE_GRAPHIC:
            DBG("graphic\n");
            break;
        case GMODE_BLANK:
        default:
            DBG("blank\n");
            break;
        }

        full_update = 1;
    }

    switch(graphic_mode) {
    case GMODE_TEXT:
        surface_draw_text(s, vram, full_update);
        break;
    case GMODE_GRAPHIC:
        surface_draw_graphic(s, vram, full_update);
        break;
    case GMODE_BLANK:
    default:
        surface_draw_blank(s, full_update);
        break;
    }

    vga_put_vram();
}

void
surface_teardown(void)
{
}

/*
 * Local variables:
 * mode: C
 * c-tab-always-indent: nil
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * c-basic-indent: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */

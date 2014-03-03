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
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <assert.h>
#include <inttypes.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include <xenctrl.h>

#include "debug.h"
#include "pci.h"
#include "demu.h"
#include "mapcache.h"
#include "vga.h"

#define DEBUG_VGA_MEMORY    0
#define DEBUG_VGA_IO        0
#define DEBUG_VBE_IO        0

/* force some bits to zero */
static const uint8_t __sr_mask[8] = {
    (uint8_t)~0xfc,
    (uint8_t)~0xc2,
    (uint8_t)~0xf0,
    (uint8_t)~0xc0,
    (uint8_t)~0xf1,
    (uint8_t)~0xff,
    (uint8_t)~0xff,
    (uint8_t)~0x00,
};

static const uint8_t __gr_mask[16] = {
    (uint8_t)~0xf0, /* 0x00 */
    (uint8_t)~0xf0, /* 0x01 */
    (uint8_t)~0xf0, /* 0x02 */
    (uint8_t)~0xe0, /* 0x03 */
    (uint8_t)~0xfc, /* 0x04 */
    (uint8_t)~0x84, /* 0x05 */
    (uint8_t)~0xf0, /* 0x06 */
    (uint8_t)~0xf0, /* 0x07 */
    (uint8_t)~0x00, /* 0x08 */
    (uint8_t)~0xff, /* 0x09 */
    (uint8_t)~0xff, /* 0x0a */
    (uint8_t)~0xff, /* 0x0b */
    (uint8_t)~0xff, /* 0x0c */
    (uint8_t)~0xff, /* 0x0d */
    (uint8_t)~0xff, /* 0x0e */
    (uint8_t)~0xff, /* 0x0f */
};

#define PAT(x) (x)

#define GET_PLANE(data, p) (((data) >> ((p) * 8)) & 0xff)

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

typedef struct vga {
    int             fd;

    uint64_t        vram_addr;
    uint64_t        vram_size;
    uint8_t         *vram;
    pthread_mutex_t vram_lock;
    unsigned long   *bitmap;

    uint64_t        mmio_addr;
    uint64_t        mmio_size;

    uint64_t        rom_addr;
    uint64_t        rom_size;
    uint8_t         *rom;

    uint64_t        lfb_addr;
    uint64_t        lfb_size;

    uint32_t        latch;
    uint8_t         sr_index;
    uint8_t         sr[256];
    uint8_t         gr_index;
    uint8_t         gr[256];
    uint8_t         ar_index;
    uint8_t         ar[21];
    int32_t         ar_flip_flop;
    uint8_t         cr_index;
    uint8_t         cr[256];        /* CRT registers */
    uint8_t         msr;            /* Misc Output Register */
    uint8_t         fcr;            /* Feature Control Register */
    uint8_t         st00;           /* status 0 */
    uint8_t         st01;           /* status 1 */

    uint8_t         dac_state;
    uint8_t         dac_sub_index;
    uint8_t         dac_read_index;
    uint8_t         dac_write_index;
    uint8_t         dac_cache[3];   /* Used when writing */
    int32_t         dac_8bit;

    uint8_t         palette[768];

    int32_t         bank_offset;

    uint16_t        vbe_index;
    uint16_t        vbe_regs[VBE_DISPI_INDEX_NB];
    uint32_t        vbe_start_addr;
    uint32_t        vbe_line_offset;
    uint32_t        vbe_bank_mask;

    uint32_t        plane_updated;
} vga_t;

static  vga_t   vga_state;

static void
vga_bios_init(void)
{
    uint8_t palette_model[192] = {
        0,   0,   0,   0,   0, 170,   0, 170,
        0,   0, 170, 170, 170,   0,   0, 170,
        0, 170, 170,  85,   0, 170, 170, 170,
       85,  85,  85,  85,  85, 255,  85, 255,
       85,  85, 255, 255, 255,  85,  85, 255,
       85, 255, 255, 255,  85, 255, 255, 255,
        0,  21,   0,   0,  21,  42,   0,  63,
        0,   0,  63,  42,  42,  21,   0,  42,
       21,  42,  42,  63,   0,  42,  63,  42,
        0,  21,  21,   0,  21,  63,   0,  63,
       21,   0,  63,  63,  42,  21,  21,  42,
       21,  63,  42,  63,  21,  42,  63,  63,
       21,   0,   0,  21,   0,  42,  21,  42,
        0,  21,  42,  42,  63,   0,   0,  63,
        0,  42,  63,  42,   0,  63,  42,  42,
       21,   0,  21,  21,   0,  63,  21,  42,
       21,  21,  42,  63,  63,   0,  21,  63,
        0,  63,  63,  42,  21,  63,  42,  63,
       21,  21,   0,  21,  21,  42,  21,  63,
        0,  21,  63,  42,  63,  21,   0,  63,
       21,  42,  63,  63,   0,  63,  63,  42,
       21,  21,  21,  21,  21,  63,  21,  63,
       21,  21,  63,  63,  63,  21,  21,  63,
       21,  63,  63,  63,  21,  63,  63,  63
    };

    vga_state.latch = 0;

    vga_state.sr_index = 3;
    vga_state.sr[0] = 3;
    vga_state.sr[1] = 0;
    vga_state.sr[2] = 3;
    vga_state.sr[3] = 0;
    vga_state.sr[4] = 2;
    vga_state.sr[5] = 0;
    vga_state.sr[6] = 0;
    vga_state.sr[7] = 0;

    vga_state.gr_index = 5;
    vga_state.gr[0] = 0;
    vga_state.gr[1] = 0;
    vga_state.gr[2] = 0;
    vga_state.gr[3] = 0;
    vga_state.gr[4] = 0;
    vga_state.gr[5] = 16;
    vga_state.gr[6] = 14;
    vga_state.gr[7] = 15;
    vga_state.gr[8] = 255;

    /* changed by out 0x03c0 */
    vga_state.ar_index = 32;
    vga_state.ar[0] = 0;
    vga_state.ar[1] = 1;
    vga_state.ar[2] = 2;
    vga_state.ar[3] = 3;
    vga_state.ar[4] = 4;
    vga_state.ar[5] = 5;
    vga_state.ar[6] = 6;
    vga_state.ar[7] = 7;
    vga_state.ar[8] = 8;
    vga_state.ar[9] = 9;
    vga_state.ar[10] = 10;
    vga_state.ar[11] = 11;
    vga_state.ar[12] = 12;
    vga_state.ar[13] = 13;
    vga_state.ar[14] = 14;
    vga_state.ar[15] = 15;
    vga_state.ar[16] = 12;
    vga_state.ar[17] = 0;
    vga_state.ar[18] = 15;
    vga_state.ar[19] = 8;
    vga_state.ar[20] = 0;

    vga_state.ar_flip_flop = 1;

    vga_state.cr_index = 15;
    vga_state.cr[0] = 95;
    vga_state.cr[1] = 79;
    vga_state.cr[2] = 80;
    vga_state.cr[3] = 130;
    vga_state.cr[4] = 85;
    vga_state.cr[5] = 129;
    vga_state.cr[6] = 191;
    vga_state.cr[7] = 31;
    vga_state.cr[8] = 0;
    vga_state.cr[9] = 79;
    vga_state.cr[10] = 14;
    vga_state.cr[11] = 15;
    vga_state.cr[12] = 0;
    vga_state.cr[13] = 0;
    vga_state.cr[14] = 5;
    vga_state.cr[15] = 160;
    vga_state.cr[16] = 156;
    vga_state.cr[17] = 142;
    vga_state.cr[18] = 143;
    vga_state.cr[19] = 40;
    vga_state.cr[20] = 31;
    vga_state.cr[21] = 150;
    vga_state.cr[22] = 185;
    vga_state.cr[23] = 163;
    vga_state.cr[24] = 255;

    vga_state.msr = 103;
    vga_state.fcr = 0;
    vga_state.st00 = 0;
    vga_state.st01 = 0;

    /* dac_* & palette will be initialized by os through out 0x03c8 &
     * out 0c03c9(1:3) */
    vga_state.dac_state = 0;
    vga_state.dac_sub_index = 0;
    vga_state.dac_read_index = 0;
    vga_state.dac_write_index = 16;
    vga_state.dac_cache[0] = 255;
    vga_state.dac_cache[1] = 255;
    vga_state.dac_cache[2] = 255;

    /* palette */
    memcpy(vga_state.palette, palette_model, 192);

    vga_state.bank_offset = 0;
}

static void
vga_reset(void)
{
    vga_state.lfb_addr = 0;
    vga_state.lfb_size = 0;
    vga_state.sr_index = 0;
    memset(vga_state.sr, '\0', sizeof(vga_state.sr));
    vga_state.gr_index = 0;
    memset(vga_state.gr, '\0', sizeof(vga_state.gr));
    vga_state.ar_index = 0;
    memset(vga_state.ar, '\0', sizeof(vga_state.ar));
    vga_state.ar_flip_flop = 0;
    vga_state.cr_index = 0;
    memset(vga_state.cr, '\0', sizeof(vga_state.cr));
    vga_state.msr = 0;
    vga_state.fcr = 0;
    vga_state.st00 = 0;
    vga_state.st01 = 0;
    vga_state.dac_state = 0;
    vga_state.dac_sub_index = 0;
    vga_state.dac_read_index = 0;
    vga_state.dac_write_index = 0;
    memset(vga_state.dac_cache, '\0', sizeof(vga_state.dac_cache));
    vga_state.dac_8bit = 0;
    memset(vga_state.palette, '\0', sizeof(vga_state.palette));
    vga_state.bank_offset = 0;
    vga_state.vbe_index = 0;
    memset(vga_state.vbe_regs, '\0', sizeof(vga_state.vbe_regs));
    vga_state.vbe_regs[VBE_DISPI_INDEX_ID] = VBE_DISPI_ID0;
    vga_state.vbe_start_addr = 0;
    vga_state.vbe_line_offset = 0;
    vga_state.vbe_bank_mask = (vga_state.vram_size >> 16) - 1;
    vga_state.plane_updated = 0;

    vga_bios_init();
}

static uint8_t
vga_port_readb(void *priv, uint64_t addr)
{
    uint8_t index;
    uint8_t val;

    assert(priv == NULL);

    /* check port range access depending on color/monochrome mode */
    if ((addr >= 0x3b0 && addr <= 0x3bf && (vga_state.msr & MSR_COLOR_EMULATION)) ||
        (addr >= 0x3d0 && addr <= 0x3df && !(vga_state.msr & MSR_COLOR_EMULATION))) {
        val = 0xff;
    } else {
        switch(addr) {
        case 0x3c0:
            if (vga_state.ar_flip_flop == 0) {
                val = vga_state.ar_index;
            } else {
                val = 0;
            }
            break;
        case 0x3c1:
            index = vga_state.ar_index & 0x1f;
            if (index < 21)
                val = vga_state.ar[index];
            else
                val = 0;
            break;
        case 0x3c2:
            val = vga_state.st00;
            break;
        case 0x3c4:
            val = vga_state.sr_index;
            break;
        case 0x3c5:
            val = vga_state.sr[vga_state.sr_index];
            break;
        case 0x3c7:
            val = vga_state.dac_state;
            break;
	case 0x3c8:
	    val = vga_state.dac_write_index;
	    break;
        case 0x3c9:
            val = vga_state.palette[vga_state.dac_read_index * 3 + vga_state.dac_sub_index];
            if (++vga_state.dac_sub_index == 3) {
                vga_state.dac_sub_index = 0;
                vga_state.dac_read_index++;
            }
            break;
        case 0x3ca:
            val = vga_state.fcr;
            break;
        case 0x3cc:
            val = vga_state.msr;
            break;
        case 0x3ce:
            val = vga_state.gr_index;
            break;
        case 0x3cf:
            val = vga_state.gr[vga_state.gr_index];
            break;
        case 0x3b4:
        case 0x3d4:
            val = vga_state.cr_index;
            break;
        case 0x3b5:
        case 0x3d5:
            val = vga_state.cr[vga_state.cr_index];
            break;
        case 0x3ba:
        case 0x3da:
            /* just toggle to fool polling */
            vga_state.st01 ^= (ST01_V_RETRACE | ST01_DISP_ENABLE);
            val = vga_state.st01;
            vga_state.ar_flip_flop = 0;
            break;
        default:
            val = 0x00;
            break;
        }
    }

#if  DEBUG_VGA_IO
    DBG("[0x%"PRIx64"] -> 0x%02x\n", addr, val);
#endif

    return val;
}

static void
vga_port_writeb(void *priv, uint64_t addr, uint8_t val)
{
    uint8_t index;

    assert(priv == NULL);

#if  DEBUG_VGA_IO
    DBG("[0x%"PRIx64"] <- 0x%02x\n", addr, val);
#endif

    /* check port range access depending on color/monochrome mode */
    if ((addr >= 0x3b0 && addr <= 0x3bf && (vga_state.msr & MSR_COLOR_EMULATION)) ||
        (addr >= 0x3d0 && addr <= 0x3df && !(vga_state.msr & MSR_COLOR_EMULATION)))
        return;

    switch(addr) {
    case 0x3c0:
        if (vga_state.ar_flip_flop == 0) {
            val &= 0x3f;
            vga_state.ar_index = val;
        } else {
            index = vga_state.ar_index & 0x1f;
            switch(index) {
            case 0x00 ... 0x0f:
                vga_state.ar[index] = val & 0x3f;
                break;
            case 0x10:
                vga_state.ar[index] = val & ~0x10;
                break;
            case 0x11:
                vga_state.ar[index] = val;
                break;
            case 0x12:
                vga_state.ar[index] = val & ~0xc0;
                break;
            case 0x13:
                vga_state.ar[index] = val & ~0xf0;
                break;
            case 0x14:
                vga_state.ar[index] = val & ~0xf0;
                break;
            default:
                break;
            }
        }
        vga_state.ar_flip_flop ^= 1;
        break;
    case 0x3c2:
        vga_state.msr = val & ~0x10;
        break;
    case 0x3c4:
        vga_state.sr_index = val & 7;
        break;
    case 0x3c5:
        vga_state.sr[vga_state.sr_index] = val & __sr_mask[vga_state.sr_index];
        break;
    case 0x3c7:
        vga_state.dac_read_index = val;
        vga_state.dac_sub_index = 0;
        vga_state.dac_state = 3;
        break;
    case 0x3c8:
        vga_state.dac_write_index = val;
        vga_state.dac_sub_index = 0;
        vga_state.dac_state = 0;
        break;
    case 0x3c9:
        vga_state.dac_cache[vga_state.dac_sub_index] = val;
        if (++vga_state.dac_sub_index == 3) {
            memcpy(&vga_state.palette[vga_state.dac_write_index * 3], vga_state.dac_cache, 3);
            vga_state.dac_sub_index = 0;
            vga_state.dac_write_index++;
        }
        break;
    case 0x3ce:
        vga_state.gr_index = val & 0x0f;
        break;
    case 0x3cf:
        vga_state.gr[vga_state.gr_index] = val & __gr_mask[vga_state.gr_index];
        break;
    case 0x3b4:
    case 0x3d4:
        vga_state.cr_index = val;
        break;
    case 0x3b5:
    case 0x3d5:
        /* handle CR0-7 protection */
        if ((vga_state.cr[0x11] & 0x80) && vga_state.cr_index <= 7) {
            /* can always write bit 4 of CR7 */
            if (vga_state.cr_index == 7)
                vga_state.cr[7] = (vga_state.cr[7] & ~0x10) | (val & 0x10);
            return;
        }
        switch(vga_state.cr_index) {
        case 0x01: /* horizontal display end */
        case 0x07:
        case 0x09:
        case 0x0c:
        case 0x0d:
        case 0x12: /* vertical display end */
            vga_state.cr[vga_state.cr_index] = val;
            break;
        default:
            vga_state.cr[vga_state.cr_index] = val;
            break;
        }

        break;
    case 0x3ba:
    case 0x3da:
        vga_state.fcr = val & 0x10;
        break;
    }
}

static io_ops_t vga_port_ops  = {
    .readb = vga_port_readb,
    .writeb = vga_port_writeb
};

static void
__copy_from_vram(uint64_t addr, uint8_t *dst, uint64_t size)
{
    if (vga_state.vram != NULL)
        memcpy(dst, &vga_state.vram[addr], size);
    else
        memset(dst, 0xff, size);
}

static uint8_t
vga_memory_readb(void *priv, uint64_t addr)
{
    uint8_t memory_map_mode;
    uint8_t plane;
    uint8_t val;

    assert(priv == NULL);

    /* convert to VGA memory offset */
    memory_map_mode = (vga_state.gr[6] >> 2) & 3;
    addr &= 0x1ffff;

    switch(memory_map_mode) {
    case 0:
        break;
    case 1:
        if (addr >= 0x10000)
            return 0xff;
        addr += vga_state.bank_offset;
        break;
    case 2:
        addr -= 0x10000;
        if (addr >= 0x8000)
            return 0xff;
        break;
    default:
    case 3:
        addr -= 0x18000;
        if (addr >= 0x8000)
            return 0xff;
        break;
    }

    if (vga_state.sr[4] & 0x08) {
        /* chain 4 mode : simplest access */
        __copy_from_vram(addr, &val, 1);
    } else if (vga_state.gr[5] & 0x10) {
        /* odd/even mode (aka text mode mapping) */
        plane = (vga_state.gr[4] & 2) | (addr & 1);
        __copy_from_vram(((addr & ~1) << 1) | plane, &val, 1);
    } else {
        /* standard VGA latched access */
        __copy_from_vram(addr << 2, (uint8_t *)&vga_state.latch, 4);

        if (!(vga_state.gr[5] & 0x08)) {
            /* read mode 0 */
            plane = vga_state.gr[4];
            val = GET_PLANE(vga_state.latch, plane);
        } else {
            uint32_t    tmp;
            /* read mode 1 */
            tmp = (vga_state.latch ^ mask16[vga_state.gr[2]]) & mask16[vga_state.gr[7]];
            tmp |= tmp >> 16;
            tmp |= tmp >> 8;
            val = (~tmp) & 0xff;
        }
    }

#if  DEBUG_VGA_MEMORY
    DBG("[0x%"PRIx64"] -> 0x%02x\n", addr, val);
#endif

    return val;
}

static void
__copy_to_vram(uint8_t *src, uint64_t addr, uint64_t size)
{
    if (vga_state.vram != NULL) {
        memcpy(&vga_state.vram[addr], src, size);
        demu_set_guest_dirty_page((vga_state.vram_addr + addr) >> TARGET_PAGE_SHIFT);
    }
}

static void
vga_memory_writeb(void *priv, uint64_t addr, uint8_t val)
{
    uint8_t memory_map_mode;
    uint8_t plane;
    uint8_t mask;

    assert(priv == NULL);

#if  DEBUG_VGA_MEMORY
    DBG("[0x%"PRIx64"] <- 0x%02x\n", addr, val);
#endif

    /* convert to VGA memory offset */
    memory_map_mode = (vga_state.gr[6] >> 2) & 3;
    addr &= 0x1ffff;

    switch(memory_map_mode) {
    case 0:
        break;
    case 1:
        if (addr >= 0x10000)
            return;
        addr += vga_state.bank_offset;
        break;
    case 2:
        addr -= 0x10000;
        if (addr >= 0x8000)
            return;
        break;
    default:
    case 3:
        addr -= 0x18000;
        if (addr >= 0x8000)
            return;
        break;
    }

    if (vga_state.sr[4] & 0x08) {
        /* chain 4 mode : simplest access */
        plane = addr & 3;
        mask = (1 << plane);
        if (vga_state.sr[2] & mask) {
            __copy_to_vram(&val, addr, 1);
#if  DEBUG_VGA_MEMORY
            DBG("chain4: [0x%"PRIx64"] val=0x%02x\n", addr, val);
#endif
            vga_state.plane_updated |= mask; /* only used to detect font change */

        }
    } else if (vga_state.gr[5] & 0x10) {
        /* odd/even mode (aka text mode mapping) */
        plane = (vga_state.gr[4] & 2) | (addr & 1);
        mask = (1 << plane);
        if (vga_state.sr[2] & mask) {
            addr = ((addr & ~1) << 1) | plane;
            __copy_to_vram(&val, addr, 1);
#if  DEBUG_VGA_MEMORY
            DBG("odd/even: [0x%"PRIx64"] val=0x%02x\n", addr, val);
#endif
            vga_state.plane_updated |= mask; /* only used to detect font change */
        }
    } else {
        uint8_t     write_mode;
        uint8_t     func_select;
        uint8_t     b;
        uint32_t    write_mask;
        uint32_t    bit_mask;
        uint32_t    set_mask;
        uint32_t    write_val;
        uint32_t    tmp;

        write_val = val;

        /* standard VGA latched access */
        write_mode = vga_state.gr[5] & 3;
        switch(write_mode) {
        default:
        case 0:
            /* rotate */
            b = vga_state.gr[3] & 7;
            write_val = ((write_val >> b) | (write_val << (8 - b))) & 0xff;
            write_val |= write_val << 8;
            write_val |= write_val << 16;

            /* apply set/reset mask */
            set_mask = mask16[vga_state.gr[1]];
            write_val = (write_val & ~set_mask) | (mask16[vga_state.gr[0]] & set_mask);
            bit_mask = vga_state.gr[8];
            break;
        case 1:
            write_val = vga_state.latch;
            goto do_write;
        case 2:
            write_val = mask16[write_val & 0x0f];
            bit_mask = vga_state.gr[8];
            break;
        case 3:
            /* rotate */
            b = vga_state.gr[3] & 7;
            write_val = (write_val >> b) | (write_val << (8 - b));

            bit_mask = vga_state.gr[8] & write_val;
            write_val = mask16[vga_state.gr[0]];
            break;
        }

        /* apply logical operation */
        func_select = vga_state.gr[3] >> 3;
        switch(func_select) {
        case 0:
        default:
            /* nothing to do */
            break;
        case 1:
            /* and */
            write_val &= vga_state.latch;
            break;
        case 2:
            /* or */
            write_val |= vga_state.latch;
            break;
        case 3:
            /* xor */
            write_val ^= vga_state.latch;
            break;
        }

        /* apply bit mask */
        bit_mask |= bit_mask << 8;
        bit_mask |= bit_mask << 16;
        write_val = (write_val & bit_mask) | (vga_state.latch & ~bit_mask);

    do_write:
        /* mask data according to sr[2] */
        mask = vga_state.sr[2];
        vga_state.plane_updated |= mask; /* only used to detect font change */
        write_mask = mask16[mask];

        __copy_from_vram(addr << 2, (uint8_t *)&tmp, 4);
        tmp &= ~write_mask;
        tmp |= write_val & write_mask;
        __copy_to_vram((uint8_t *)&tmp, addr << 2, 4);

#if  DEBUG_VGA_MEMORY
        DBG("latch: [0x%"PRIx64"] val=0x%08x\n", addr << 2, tmp);
#endif

    }
}

static io_ops_t vga_memory_ops = {
    .readb = vga_memory_readb,
    .writeb = vga_memory_writeb
};

static uint16_t
vbe_index_read(void *priv)
{
    return vga_state.vbe_index;
}

static uint16_t
vbe_data_read(void *priv)
{
    uint16_t    val;

    if (vga_state.vbe_index <= VBE_DISPI_INDEX_NB) {
        if (vga_state.vbe_regs[VBE_DISPI_INDEX_ENABLE] & VBE_DISPI_GETCAPS) {
            switch(vga_state.vbe_index) {
            case VBE_DISPI_INDEX_XRES:
                val = VBE_DISPI_MAX_XRES;
                break;
            case VBE_DISPI_INDEX_YRES:
                val = VBE_DISPI_MAX_YRES;
                break;
            case VBE_DISPI_INDEX_BPP:
                val = VBE_DISPI_MAX_BPP;
                break;
            default:
                val = vga_state.vbe_regs[vga_state.vbe_index];
                break;
            }
        } else {
            val = vga_state.vbe_regs[vga_state.vbe_index];
        }
    } else {
        val = 0;
    }

    return val;
}

static uint16_t
vbe_port_readw(void *priv, uint64_t addr)
{
    uint16_t val;

    assert(priv == NULL);

    switch (addr) {
    case 0x1ce:
    case 0xff80:
        val = vbe_index_read(priv);
        break;

    case 0x1cf:
    case 0xff81:
        val = vbe_data_read(priv);
        break;

    default:
        assert(FALSE);
        val = 0xff;
        break;
    }

#if  DEBUG_VBE_IO
    DBG("[0x%"PRIx64"] -> 0x%04x\n", addr, val);
#endif

    return val;
}

static void
vbe_index_write(void *priv, uint16_t val)
{
    assert(priv == NULL);

    vga_state.vbe_index = val;
}

static void
__clear_vram(uint64_t size)
{
    if (vga_state.vram != NULL)
        memset(vga_state.vram, 0, size);
}

static void
vbe_data_write(void *priv, uint16_t val)
{
    assert(priv == NULL);

    if (vga_state.vbe_index <= VBE_DISPI_INDEX_NB) {
        switch(vga_state.vbe_index) {
        case VBE_DISPI_INDEX_ID:
            if (val == VBE_DISPI_ID0 ||
                val == VBE_DISPI_ID1 ||
                val == VBE_DISPI_ID2 ||
                val == VBE_DISPI_ID3 ||
                val == VBE_DISPI_ID4) {
                vga_state.vbe_regs[vga_state.vbe_index] = val;
            }
            break;
        case VBE_DISPI_INDEX_XRES:
            if ((val <= VBE_DISPI_MAX_XRES) && ((val & 7) == 0)) {
                vga_state.vbe_regs[vga_state.vbe_index] = val;
            }
            break;
        case VBE_DISPI_INDEX_YRES:
            if (val <= VBE_DISPI_MAX_YRES) {
                vga_state.vbe_regs[vga_state.vbe_index] = val;
            }
            break;
        case VBE_DISPI_INDEX_BPP:
            if (val == 0)
                val = 8;
            if (val == 4 || val == 8 || val == 15 ||
                val == 16 || val == 24 || val == 32) {
                vga_state.vbe_regs[vga_state.vbe_index] = val;
            }
            break;
        case VBE_DISPI_INDEX_BANK:
            if (vga_state.vbe_regs[VBE_DISPI_INDEX_BPP] == 4) {
              val &= (vga_state.vbe_bank_mask >> 2);
            } else {
              val &= vga_state.vbe_bank_mask;
            }
            vga_state.vbe_regs[vga_state.vbe_index] = val;
            vga_state.bank_offset = (val << 16);
            break;
        case VBE_DISPI_INDEX_ENABLE:
            if ((val & VBE_DISPI_ENABLED) &&
                !(vga_state.vbe_regs[VBE_DISPI_INDEX_ENABLE] & VBE_DISPI_ENABLED)) {
                int h, shift_control;
                
                vga_state.vbe_regs[VBE_DISPI_INDEX_VIRT_WIDTH] =
                    vga_state.vbe_regs[VBE_DISPI_INDEX_XRES];
                vga_state.vbe_regs[VBE_DISPI_INDEX_VIRT_HEIGHT] =
                    vga_state.vbe_regs[VBE_DISPI_INDEX_YRES];
                vga_state.vbe_regs[VBE_DISPI_INDEX_X_OFFSET] = 0;
                vga_state.vbe_regs[VBE_DISPI_INDEX_Y_OFFSET] = 0;

                if (vga_state.vbe_regs[VBE_DISPI_INDEX_BPP] == 4)
                    vga_state.vbe_line_offset = vga_state.vbe_regs[VBE_DISPI_INDEX_XRES] >> 1;
                else
                    vga_state.vbe_line_offset = vga_state.vbe_regs[VBE_DISPI_INDEX_XRES] *
                        ((vga_state.vbe_regs[VBE_DISPI_INDEX_BPP] + 7) >> 3);
                vga_state.vbe_start_addr = 0;

                /* clear the screen (should be done in BIOS) */
                if (!(val & VBE_DISPI_NOCLEARMEM)) {
                    __clear_vram(vga_state.vbe_regs[VBE_DISPI_INDEX_YRES] * vga_state.vbe_line_offset);
                }

                /* we initialize the VGA graphic mode (should be done
                   in BIOS) */
                vga_state.gr[0x06] = (vga_state.gr[0x06] & ~0x0c) | 0x05; /* graphic mode + memory map 1 */
                vga_state.cr[0x17] |= 3; /* no CGA modes */
                vga_state.cr[0x13] = vga_state.vbe_line_offset >> 3;
                /* width */
                vga_state.cr[0x01] = (vga_state.vbe_regs[VBE_DISPI_INDEX_XRES] >> 3) - 1;
                /* height (only meaningful if < 1024) */
                h = vga_state.vbe_regs[VBE_DISPI_INDEX_YRES] - 1;
                vga_state.cr[0x12] = h;
                vga_state.cr[0x07] = (vga_state.cr[0x07] & ~0x42) |
                    ((h >> 7) & 0x02) | ((h >> 3) & 0x40);
                /* line compare to 1023 */
                vga_state.cr[0x18] = 0xff;
                vga_state.cr[0x07] |= 0x10;
                vga_state.cr[0x09] |= 0x40;

                if (vga_state.vbe_regs[VBE_DISPI_INDEX_BPP] == 4) {
                    shift_control = 0;
                    vga_state.sr[0x01] &= ~8; /* no double line */
                } else {
                    shift_control = 2;
                    vga_state.sr[4] |= 0x08; /* set chain 4 mode */
                    vga_state.sr[2] |= 0x0f; /* activate all planes */
                }
                vga_state.gr[0x05] = (vga_state.gr[0x05] & ~0x60) | (shift_control << 5);
                vga_state.cr[0x09] &= ~0x9f; /* no double scan */
            } else {
                /* XXX: the bios should do that */
                vga_state.bank_offset = 0;
            }
            vga_state.dac_8bit = (val & VBE_DISPI_8BIT_DAC) > 0;
            vga_state.vbe_regs[vga_state.vbe_index] = val;
            break;
        case VBE_DISPI_INDEX_VIRT_WIDTH:
            {
                int w, h, line_offset;

                if (val < vga_state.vbe_regs[VBE_DISPI_INDEX_XRES])
                    return;
                w = val;
                if (vga_state.vbe_regs[VBE_DISPI_INDEX_BPP] == 4)
                    line_offset = w >> 1;
                else
                    line_offset = w * ((vga_state.vbe_regs[VBE_DISPI_INDEX_BPP] + 7) >> 3);
                h = vga_state.vram_size / line_offset;
                if (h < vga_state.vbe_regs[VBE_DISPI_INDEX_YRES])
                    return;
                vga_state.vbe_regs[VBE_DISPI_INDEX_VIRT_WIDTH] = w;
                vga_state.vbe_regs[VBE_DISPI_INDEX_VIRT_HEIGHT] = h;
                vga_state.vbe_line_offset = line_offset;
            }
            break;
        case VBE_DISPI_INDEX_X_OFFSET:
        case VBE_DISPI_INDEX_Y_OFFSET:
            {
                int x;
                vga_state.vbe_regs[vga_state.vbe_index] = val;
                vga_state.vbe_start_addr = vga_state.vbe_line_offset * vga_state.vbe_regs[VBE_DISPI_INDEX_Y_OFFSET];
                x = vga_state.vbe_regs[VBE_DISPI_INDEX_X_OFFSET];
                if (vga_state.vbe_regs[VBE_DISPI_INDEX_BPP] == 4)
                    vga_state.vbe_start_addr += x >> 1;
                else
                    vga_state.vbe_start_addr += x * ((vga_state.vbe_regs[VBE_DISPI_INDEX_BPP] + 7) >> 3);
                vga_state.vbe_start_addr >>= 2;
            }
            break;
        default:
            break;
        }
    }
}

static void
vbe_port_writew(void *priv, uint64_t addr, uint16_t val)
{
    assert(priv == NULL);

#if  DEBUG_VBE_IO
    DBG("[0x%"PRIx64"] <- 0x%04x\n", addr, val);
#endif

    switch (addr) {
    case 0x1ce:
    case 0xff80:
        vbe_index_write(priv, val);
        break;

    case 0x1cf:
    case 0xff81:
        vbe_data_write(priv, val);
        break;

    default:
        assert(FALSE);
        break;
    }
}

static io_ops_t vbe_port_ops = {
    .readw = vbe_port_readw,
    .writew = vbe_port_writew
};

static int
vga_register(void)
{
    int     rc;

    rc = demu_register_port_space(0x3c0, 16, &vga_port_ops, NULL);
    if (rc < 0)
        goto fail1;

    rc = demu_register_port_space(0x3b4, 2, &vga_port_ops, NULL);
    if (rc < 0)
        goto fail2;

    rc = demu_register_port_space(0x3d4, 2, &vga_port_ops, NULL);
    if (rc < 0)
        goto fail3;

    rc = demu_register_port_space(0x3ba, 1, &vga_port_ops, NULL);
    if (rc < 0)
        goto fail4;

    rc = demu_register_port_space(0x3da, 1, &vga_port_ops, NULL);
    if (rc < 0)
        goto fail5;

    vga_state.vbe_regs[VBE_DISPI_INDEX_ID] = VBE_DISPI_ID0;
    vga_state.vbe_bank_mask = ((vga_state.vram_size >> 16) - 1);

    rc = demu_register_port_space(0x1ce, 2, &vbe_port_ops, NULL);
    if (rc < 0)
        goto fail6;

    rc = demu_register_port_space(0xff80, 2, &vbe_port_ops, NULL);
    if (rc < 0)
        goto fail7;

    rc = demu_register_memory_space(0xa0000, 0x20000, &vga_memory_ops, NULL);
    if (rc < 0)
        goto fail8;

    return 0;

fail8:
    DBG("fail8\n");

    demu_deregister_port_space(0xff80);

fail7:
    DBG("fail7\n");

    demu_deregister_port_space(0x1ce);

fail6:
    DBG("fail6\n");

    demu_deregister_port_space(0x3da);

fail5:
    DBG("fail5\n");

    demu_deregister_port_space(0x3ba);

fail4:
    DBG("fail4\n");

    demu_deregister_port_space(0x3d4);

fail3:
    DBG("fail3\n");

    demu_deregister_port_space(0x3b4);

fail2:
    DBG("fail2\n");

    demu_deregister_port_space(0x3c0);

fail1:
    DBG("fail1\n");

    return -1;
}

static void
vga_deregister(void)
{
    demu_deregister_memory_space(0xa0000);

    demu_deregister_port_space(0xff80);
    demu_deregister_port_space(0x1ce);
    demu_deregister_port_space(0x3da);
    demu_deregister_port_space(0x3ba);
    demu_deregister_port_space(0x3d4);
    demu_deregister_port_space(0x3b4);
    demu_deregister_port_space(0x3c0);
}

static void
vram_bar_enable(void *priv, uint64_t addr)
{
    assert(priv == NULL);

    vga_state.vram_addr = addr;
    DBG("%"PRIx64"\n", vga_state.vram_addr);

    pthread_mutex_lock(&vga_state.vram_lock);

    vga_state.vram = demu_map_guest_range(vga_state.vram_addr,
                                          vga_state.vram_size,
                                          TRUE);

    pthread_mutex_unlock(&vga_state.vram_lock);

    vga_state.lfb_addr = vga_state.vram_addr;
    vga_state.lfb_size = vga_state.vram_size;

    vga_state.vbe_regs[VBE_DISPI_INDEX_LFB_ADDRESS_H] = vga_state.lfb_addr >> 16;
    vga_state.vbe_regs[VBE_DISPI_INDEX_LFB_ADDRESS_L] = vga_state.lfb_addr & 0xFFFF;
    vga_state.vbe_regs[VBE_DISPI_INDEX_VIDEO_MEMORY_64K] = vga_state.lfb_size >> 16;
}

static void
vram_bar_disable(void *priv)
{
    assert(priv == FALSE);

    DBG("%"PRIx64"\n", vga_state.vram_addr);

    demu_track_dirty_vram(0, 0, NULL);

    pthread_mutex_lock(&vga_state.vram_lock);

    if (vga_state.vram != NULL) {
        demu_unmap_guest_range(vga_state.vram,
                               vga_state.vram_addr,
                               vga_state.vram_size,
                               TRUE);
        vga_state.vram = NULL;
    }

    pthread_mutex_unlock(&vga_state.vram_lock);
}

#define MMIO_BAR_VGA_OFFSET 0x400
#define MMIO_BAR_VGA_SIZE   (0x3e0 - 0x3c0)
#define MMIO_BAR_VBE_OFFSET 0x500
#define MMIO_BAR_VBE_SIZE   (0x0b * 2)
#define MMIO_BAR_SIZE       0x1000

static uint8_t
mmio_bar_vga_readb(void *priv, uint64_t addr)
{
    addr -= vga_state.mmio_addr + MMIO_BAR_VGA_OFFSET;

    return vga_port_readb(priv, addr + 0x3c0);
}

static void
mmio_bar_vga_writeb(void *priv, uint64_t addr, uint8_t val)
{
    addr -= vga_state.mmio_addr + MMIO_BAR_VGA_OFFSET;

    vga_port_writeb(priv, addr + 0x3c0, val);
}

static io_ops_t mmio_bar_vga_ops = {
    .readb = mmio_bar_vga_readb,
    .writeb = mmio_bar_vga_writeb
};

static uint16_t
mmio_bar_vbe_readw(void *priv, uint64_t addr)
{
    addr -= vga_state.mmio_addr + MMIO_BAR_VBE_OFFSET;

    vbe_index_write(priv, addr >> 1);
    return vbe_data_read(priv);
}

static void
mmio_bar_vbe_writew(void *priv, uint64_t addr, uint16_t val)
{
    addr -= vga_state.mmio_addr + MMIO_BAR_VBE_OFFSET;

    vbe_index_write(priv, addr >> 1);
    vbe_data_write(priv, val);
}

static io_ops_t mmio_bar_vbe_ops = {
    .readw = mmio_bar_vbe_readw,
    .writew = mmio_bar_vbe_writew
};

static void
mmio_bar_enable(void *priv, uint64_t addr)
{
    assert(priv == NULL);

    vga_state.mmio_addr = addr;
    DBG("%"PRIx64"\n", vga_state.mmio_addr);

    (void) demu_register_memory_space(vga_state.mmio_addr + MMIO_BAR_VGA_OFFSET,
                                      MMIO_BAR_VGA_SIZE,
                                      &mmio_bar_vga_ops,
                                      NULL);

    (void)demu_register_memory_space(vga_state.mmio_addr + MMIO_BAR_VBE_OFFSET,
                                     MMIO_BAR_VBE_SIZE,
                                     &mmio_bar_vbe_ops,
                                     NULL);
}

static void
mmio_bar_disable(void *priv)
{
    assert(priv == FALSE);

    DBG("%"PRIx64"\n", vga_state.mmio_addr);

    demu_deregister_memory_space(vga_state.mmio_addr + MMIO_BAR_VGA_OFFSET);
    demu_deregister_memory_space(vga_state.mmio_addr + MMIO_BAR_VBE_OFFSET);
}

static void
__copy_from_rom(uint64_t addr, uint8_t *dst, uint64_t size)
{
    memcpy(dst, &vga_state.rom[addr], size);
}

static uint8_t
rom_readb(void *priv, uint64_t addr)
{
    uint8_t val;

    addr -= vga_state.rom_addr;

    __copy_from_rom(addr, &val, 1);

    return val;
}

static void
rom_writeb(void *priv, uint64_t addr, uint8_t val)
{
    DBG("%"PRIx64" <- %02x\n", addr, val);
}

static io_ops_t rom_bar_ops = {
    .readb = rom_readb,
    .writeb = rom_writeb
};

static void
rom_bar_enable(void *priv, uint64_t addr)
{
    assert(priv == NULL);

    vga_state.rom_addr = addr;
    DBG("%"PRIx64"\n", vga_state.rom_addr);

    (void) demu_register_memory_space(vga_state.rom_addr,
                                      vga_state.rom_size,
                                      &rom_bar_ops,
                                      NULL);
}

static void
rom_bar_disable(void *priv)
{
    assert(priv == FALSE);

    DBG("%"PRIx64"\n", vga_state.rom_addr);

    demu_deregister_memory_space(vga_state.rom_addr);
}

int
vga_initialize(unsigned int bus, unsigned int device, unsigned int function,
               uint64_t vram_size, char *romfile)
{
    pci_info_t  info;
    struct stat st;
    int         rc;

    vga_state.vram_size = vram_size;

    vga_state.bitmap = (unsigned long *)malloc(vram_size >> TARGET_PAGE_SHIFT);
    if (vga_state.bitmap == NULL)
        goto fail1;

    vga_reset();

    rc = vga_register();
    if (rc < 0)
        goto fail2;

    info.bus = bus;
    info.device = device;
    info.function = function;

    info.vendor_id = 0x1234;
    info.device_id = 0x1111;
    info.subvendor_id = 0x5853;
    info.subdevice_id = 0x0001;
    info.revision = 0x01;
    info.class = 0x03;
    info.subclass = 0x00;
    info.prog_if = 0x00;
    info.header_type = 0;
    info.command = PCI_COMMAND_IO | PCI_COMMAND_MEMORY;
    info.interrupt_pin = 1;

    rc = pci_device_register(&info);
    if (rc < 0)
        goto fail3;

    pthread_mutex_init(&vga_state.vram_lock, NULL);

    rc = pci_bar_register(0,
                          PCI_BASE_ADDRESS_SPACE_MEMORY | PCI_BASE_ADDRESS_MEM_PREFETCH,
                          vga_state.vram_size,
                          vram_bar_enable,
                          vram_bar_disable,
                          NULL);
    if (rc < 0)
        goto fail4;

    vga_state.mmio_size = MMIO_BAR_SIZE;

    rc = pci_bar_register(2,
                          PCI_BASE_ADDRESS_SPACE_MEMORY,
                          vga_state.mmio_size,
                          mmio_bar_enable,
                          mmio_bar_disable,
                          NULL);
    if (rc < 0)
        goto fail5;

    vga_state.fd = -1;

    if (romfile == NULL)
        goto done;

    vga_state.fd = open(romfile, O_RDONLY);
    if (vga_state.fd < 0)
        goto fail6;

    rc = fstat(vga_state.fd, &st);
    if (rc < 0)
        goto fail7;

    vga_state.rom_size = P2ROUNDUP(st.st_size, TARGET_PAGE_SIZE);

    DBG("ROM: %s (%"PRIu64"k)\n", romfile, vga_state.rom_size / 4);

    vga_state.rom = mmap(NULL,
                            vga_state.rom_size,
                            PROT_READ,
                            MAP_SHARED,
                            vga_state.fd,
                            0);
    if (vga_state.rom == MAP_FAILED)
        goto fail8;

    if (vga_state.rom[0] != 0x55 &&
        vga_state.rom[1] != 0xAA) {
        errno = EINVAL;
        goto fail9;
    }

    rc = pci_bar_register(PCI_ROM_SLOT,
                          0,
                          vga_state.rom_size,
                          rom_bar_enable,
                          rom_bar_disable,
                          NULL);
    if (rc < 0)
        goto fail10;

done:
    pci_device_dump();

    return 0;

fail10:
    DBG("fail10\n");

fail9:
    DBG("fail9\n");

    munmap(vga_state.rom, vga_state.rom_size);

fail8:
    DBG("fail8\n");

fail7:
    DBG("fail7\n");

    close(vga_state.fd);

fail6:
    DBG("fail6\n");

    pci_bar_deregister(2);

fail5:
    DBG("fail5\n");

    pci_bar_deregister(0);
    pthread_mutex_destroy(&vga_state.vram_lock);

fail4:
    DBG("fail4\n");

    pci_device_deregister();

fail3:
    DBG("fail3\n");

    vga_deregister();

fail2:
    DBG("fail2\n");

    free(vga_state.bitmap);

fail1:
    DBG("fail1\n");

    warn("fail");
    return -1;
}

uint8_t *
vga_get_vram(void)
{
    pthread_mutex_lock(&vga_state.vram_lock);
	return vga_state.vram;
}

void
vga_put_vram(void)
{
    pthread_mutex_unlock(&vga_state.vram_lock);
}

uint8_t
vga_get_ar_index(void)
{
    return vga_state.ar_index;
}

uint8_t
vga_get_ar(int reg)
{
    return vga_state.ar[reg];
}

uint8_t
vga_get_cr(int reg)
{
    return vga_state.cr[reg];
}

uint8_t
vga_get_sr(int reg)
{
    return vga_state.sr[reg];
}

uint8_t
vga_get_gr(int reg)
{
    return vga_state.gr[reg];
}

uint8_t
vga_get_palette(int offset)
{
    return vga_state.palette[offset];
}

int
vga_is_dac_8bit(void)
{
    return !!vga_state.dac_8bit;
}

int
vga_test_and_clear_plane2(void)
{
    uint32_t val;

    val = vga_state.plane_updated & (1 << 2);
    if (val)
        vga_state.plane_updated = 0;

    return !!val;
}

uint16_t
vga_get_vbe_regs(int reg)
{
    return vga_state.vbe_regs[reg];
}

uint32_t
vga_get_vbe_start_addr(void)
{
    return vga_state.vbe_start_addr;
}

uint32_t
vga_get_vbe_line_offset(void)
{
    return vga_state.vbe_line_offset;
}

void
vga_get_vram_dirty_map(int enable)
{
    if (enable)
        demu_track_dirty_vram(vga_state.vram_addr >> TARGET_PAGE_SHIFT,
                              vga_state.vram_size >> TARGET_PAGE_SHIFT,
                              vga_state.bitmap);
    else
        demu_track_dirty_vram(0, 0, NULL);
}

int
vga_vram_is_dirty(uint64_t addr, uint64_t size)
{
    xen_pfn_t   pfn = addr >> TARGET_PAGE_SHIFT;
    int         n = size >> TARGET_PAGE_SHIFT;
    int         dirty;

    dirty = FALSE;
    while (--n >= 0) {
        dirty |= !!(vga_state.bitmap[pfn / sizeof(unsigned long) * 8] &
                    (1ul << (pfn % sizeof(unsigned long) * 8)));
        pfn++;
    }

    return dirty;
}

void
vga_teardown(void)
{
    if (vga_state.fd >= 0) {
        pci_bar_deregister(PCI_ROM_SLOT);
        munmap(vga_state.rom, vga_state.rom_size);
        close(vga_state.fd);
    }

    pci_bar_deregister(2);
    pci_bar_deregister(0);
    pthread_mutex_destroy(&vga_state.vram_lock);
    pci_device_deregister();
    vga_deregister();
    free(vga_state.bitmap);
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

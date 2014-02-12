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

#ifndef  _VGA_H
#define  _VGA_H

#define MSR_COLOR_EMULATION 0x01
#define MSR_PAGE_SELECT     0x20

#define ST01_V_RETRACE      0x08
#define ST01_DISP_ENABLE    0x01

#define VBE_DISPI_MAX_XRES                      2560
#define VBE_DISPI_MAX_YRES                      1600
#define VBE_DISPI_MAX_BPP                       32

#define VBE_DISPI_INDEX_ID                      0x0
#define VBE_DISPI_INDEX_XRES                    0x1
#define VBE_DISPI_INDEX_YRES                    0x2
#define VBE_DISPI_INDEX_BPP                     0x3
#define VBE_DISPI_INDEX_ENABLE                  0x4
#define VBE_DISPI_INDEX_BANK                    0x5
#define VBE_DISPI_INDEX_VIRT_WIDTH              0x6
#define VBE_DISPI_INDEX_VIRT_HEIGHT             0x7
#define VBE_DISPI_INDEX_X_OFFSET                0x8
#define VBE_DISPI_INDEX_Y_OFFSET                0x9
#define VBE_DISPI_INDEX_VIDEO_MEMORY_64K        0xa
#define VBE_DISPI_INDEX_LFB_ADDRESS_H           0xb
#define VBE_DISPI_INDEX_LFB_ADDRESS_L           0xc
#define VBE_DISPI_INDEX_NB                      0xd

#define VBE_DISPI_ID0                           0xb0c0
#define VBE_DISPI_ID1                           0xb0c1
#define VBE_DISPI_ID2                           0xb0c2
#define VBE_DISPI_ID3                           0xb0c3
#define VBE_DISPI_ID4                           0xb0c4

#define VBE_DISPI_DISABLED                      0x00
#define VBE_DISPI_ENABLED                       0x01
#define VBE_DISPI_GETCAPS                       0x02
#define VBE_DISPI_8BIT_DAC                      0x20
#define VBE_DISPI_LFB_ENABLED                   0x40
#define VBE_DISPI_NOCLEARMEM                    0x80

int         vga_initialize(unsigned int bus, unsigned int device, unsigned int function,
                           uint64_t vram_size, char *romfile);
void        vga_teardown(void);

uint8_t     *vga_get_vram(void);

uint8_t     vga_get_ar_index(void);
uint8_t     vga_get_ar(int reg);
uint8_t     vga_get_cr(int reg);
uint8_t     vga_get_sr(int reg);
uint8_t     vga_get_gr(int reg);
uint8_t     vga_get_palette(int offset);
int         vga_is_dac_8bit(void);
int         vga_test_and_clear_plane2(void);
uint16_t    vga_get_vbe_regs(int reg);
uint32_t    vga_get_vbe_start_addr(void);
uint32_t    vga_get_vbe_line_offset(void);

void        vga_get_vram_dirty_map(int enable);
int         vga_vram_is_dirty(uint64_t addr, uint64_t size);

#endif  /*_VGA_H */

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


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

#include <xenctrl.h>

#ifndef  _DEMU_H
#define  _DEMU_H

#ifndef TRUE
#define TRUE    1
#endif

#ifndef FALSE
#define FALSE   0
#endif

#define TARGET_PAGE_SHIFT   12
#define TARGET_PAGE_SIZE    (1 << TARGET_PAGE_SHIFT)

#define	P2ROUNDUP(_x, _a) -(-(_x) & -(_a))

void    *demu_map_guest_pages(xen_pfn_t pfn[], unsigned int n, int populate);

static inline void *demu_map_guest_page(xen_pfn_t pfn, int populate)
{
	return demu_map_guest_pages(&pfn, 1, populate);
}

void    *demu_map_guest_range(uint64_t addr, uint64_t size, int populate);

void    demu_unmap_guest_pages(void *ptr, xen_pfn_t pfn[], unsigned int n,
                               int depopulate);

static inline void demu_unmap_guest_page(void *ptr, xen_pfn_t pfn,
                                         int depopulate)
{
	return demu_unmap_guest_pages(ptr, &pfn, 1, depopulate);
}

int     demu_unmap_guest_range(void *ptr, uint64_t addr, uint64_t size,
                               int depopulate);

void    demu_set_guest_dirty_page(xen_pfn_t pfn);
void    demu_track_dirty_vram(xen_pfn_t pfn, int n, unsigned long *bitmap);

typedef struct io_ops {
        uint8_t         (*readb)(void *priv, uint64_t addr);
        uint16_t        (*readw)(void *priv, uint64_t addr);
        uint32_t        (*readl)(void *priv, uint64_t addr);
        void            (*writeb)(void *priv, uint64_t addr, uint8_t val);
        void            (*writew)(void *priv, uint64_t addr, uint16_t val);
        void            (*writel)(void *priv, uint64_t addr, uint32_t val);
} io_ops_t;

int demu_register_pci_config_space(uint8_t bus, uint8_t device, uint8_t function,
                                   const io_ops_t *ops, void *priv);
int demu_register_port_space(uint64_t start, uint64_t size,
                             const io_ops_t *ops, void *priv);
int demu_register_memory_space(uint64_t start, uint64_t size,
                               const io_ops_t *ops, void *priv);

void demu_deregister_pci_config_space(uint8_t bus, uint8_t device, uint8_t function);
void demu_deregister_port_space(uint64_t start);
void demu_deregister_memory_space(uint64_t start);

void    demu_new_framebuffer(uint32_t width, uint32_t height, uint32_t depth);
uint8_t *demu_get_framebuffer(void);
void    demu_update_framebuffer(uint32_t x, uint32_t y, uint32_t width, uint32_t height);

#endif  /* _DEMU_H */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * c-tab-always-indent: nil
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */

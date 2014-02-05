/*  
 * Copyright (c) 2012, Citrix Systems Inc.
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

#ifndef  _DEMU_H
#define  _DEMU_H

#define TARGET_PAGE_SHIFT   12
#define TARGET_PAGE_SIZE    (1 << TARGET_PAGE_SHIFT)

void    *demu_map_guest_pages(xen_pfn_t pfn[], unsigned int n);

static inline void *demu_map_guest_page(xen_pfn_t pfn)
{
	return demu_map_guest_pages(&pfn, 1);
}

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

#endif  /* _DEMU_H */

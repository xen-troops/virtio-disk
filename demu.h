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
#include <linux/types.h>

#ifndef  _DEMU_H
#define  _DEMU_H

void    demu_set_irq(int irq, int level);

#define TARGET_PAGE_SHIFT   12
#define TARGET_PAGE_SIZE    (1 << TARGET_PAGE_SHIFT)
#define TARGET_PAGE_MASK    (~(TARGET_PAGE_SIZE-1))

#define	P2ROUNDUP(_x, _a) -(-(_x) & -(_a))

void    *demu_map_guest_range(uint64_t addr, uint64_t size, int prot);
int     demu_unmap_guest_range(void *ptr, uint64_t size);

int demu_register_memory_space(uint64_t start, uint64_t size,
    void (*mmio_fn)(u64 addr, u8 *data, u32 len, u8 is_write, void *ptr),
    void *ptr);

void demu_deregister_memory_space(uint64_t start);
int demu_register_pcidev(uint8_t bus, uint8_t device, uint8_t function);
void demu_deregister_pcidev(uint8_t bus, uint8_t device, uint8_t function);

#ifdef MAP_IN_ADVANCE
void *demu_get_host_addr(uint64_t offset);
#endif

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

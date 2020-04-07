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
#include "demu.h"
#include "mapcache.h"
#include "device.h"

/* XXX: Get values from libxl */
#define GUEST_VIRTIO_MMIO_BASE	0x02000000
#define GUEST_VIRTIO_MMIO_SIZE	0x200
#define GUEST_VIRTIO_MMIO_SPI	33

/* Magic value ("virt" string) - Read Only */
#define VIRTIO_MMIO_MAGIC_VALUE	0x000

typedef struct _device_memory_state {
    unsigned int index;
    uint64_t base;
    uint64_t size;
    uint8_t registered;
} device_memory_state_t;

static device_memory_state_t device_memory_state;

static uint8_t device_memory_readb(void *priv, uint64_t addr)
{
    device_memory_state_t *state = priv;

    addr -= state->base;

    DBG("%d: offset: 0x%lx\n", state->index, addr);

    return 0;
}

static void device_memory_writeb(void *priv, uint64_t addr, uint8_t val)
{
    device_memory_state_t *state = priv;

    addr -= state->base;

    DBG("%d: offset: 0x%lx val: 0x%x\n", state->index, addr, val);
}

static uint16_t device_memory_readw(void *priv, uint64_t addr)
{
    device_memory_state_t *state = priv;

    addr -= state->base;

    DBG("%d: offset: 0x%lx\n", state->index, addr);

    return 0;
}

static void device_memory_writew(void *priv, uint64_t addr, uint16_t val)
{
    device_memory_state_t *state = priv;

    addr -= state->base;

    DBG("%d: offset: 0x%lx val: 0x%x\n", state->index, addr, val);
}

static uint32_t device_memory_readl(void *priv, uint64_t addr)
{
    device_memory_state_t *state = priv;

    addr -= state->base;

    DBG("%d: offset: 0x%lx\n", state->index, addr);

    /* XXX: For test purposes */
    if (addr == VIRTIO_MMIO_MAGIC_VALUE)
        return  ('v' | 'i' << 8 | 'r' << 16 | 't' << 24);

    return 0;
}

static void device_memory_writel(void *priv, uint64_t addr, uint32_t val)
{
    device_memory_state_t *state = priv;

    addr -= state->base;

    DBG("%d: offset: 0x%lx: val: 0x%x\n", state->index, addr, val);
}

static io_ops_t device_memory_ops = {
    .readb = device_memory_readb,
    .writeb = device_memory_writeb,
    .readw = device_memory_readw,
    .writew = device_memory_writew,
    .readl = device_memory_readl,
    .writel = device_memory_writel
};

int device_initialize(void)
{
    int rc;

    if (device_memory_state.registered)
        return -1;

    device_memory_state.index = 0;
    device_memory_state.base = GUEST_VIRTIO_MMIO_BASE;
    device_memory_state.size = GUEST_VIRTIO_MMIO_SIZE;

    rc = demu_register_memory_space(device_memory_state.base,
                                    device_memory_state.size,
                                    &device_memory_ops,
                                    &device_memory_state);
    if (rc < 0)
        goto fail1;

    device_memory_state.registered = 1;

    return 0;

fail1:
    DBG("fail1\n");

    return -1;
}

void device_teardown(void)
{
    if (!device_memory_state.registered)
        return;

    demu_deregister_memory_space(device_memory_state.base);
    device_memory_state.registered = 0;
}

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

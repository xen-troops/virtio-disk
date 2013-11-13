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
#include <err.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <assert.h>

#include <sys/types.h>

#include <xenctrl.h>

#include "debug.h"
#include "pci.h"
#include "device.h"

#define FALSE 0

typedef struct _device_io_state {
    unsigned int    index;
    unsigned int    order;
} device_io_state_t;

static  device_io_state_t   device_io_state;

static void
device_io_map(void *priv, uint64_t addr)
{
    device_io_state_t   *state = priv;

    DBG("%d: %016"PRIx64" - %016"PRIx64"\n",
        state->index,
        addr,
        addr + (1 << state->order) - 1);
}

static void
device_io_unmap(void *priv)
{
    device_io_state_t   *state = priv;

    DBG("%d\n", state->index);
}

static uint8_t
device_io_readb(void *priv, uint64_t offset)
{
    return 0;
}

static void
device_io_writeb(void *priv, uint64_t offset, uint8_t val)
{
}

static bar_ops_t    device_io_ops = {
    .map = device_io_map,
    .unmap = device_io_unmap,
    .readb = device_io_readb,
    .writeb = device_io_writeb
};

typedef struct _device_memory_state {
    unsigned int    index;
    unsigned int    order;
} device_memory_state_t;

static  device_memory_state_t   device_memory_state;

static void
device_memory_map(void *priv, uint64_t addr)
{
    device_memory_state_t   *state = priv;

    DBG("%d: %016"PRIx64" - %016"PRIx64"\n",
        state->index,
        addr,
        addr + (1 << state->order) - 1);
}

static void
device_memory_unmap(void *priv)
{
    device_memory_state_t   *state = priv;

    DBG("%d\n", state->index);
}

static uint8_t
device_memory_readb(void *priv, uint64_t offset)
{
    return 0;
}

static void
device_memory_writeb(void *priv, uint64_t offset, uint8_t val)
{
}

static bar_ops_t    device_memory_ops = {
    .map = device_memory_map,
    .unmap = device_memory_unmap,
    .readb = device_memory_readb,
    .writeb = device_memory_writeb
};

int
device_initialize(xc_interface *xch, domid_t domid, ioservid_t ioservid,
                  unsigned int bus, unsigned int device, unsigned int function)
{
    pci_info_t  info;
    int         rc;

    info.bus = bus;
    info.device = device;
    info.function = function;

    info.vendor_id = 0x5853;
    info.device_id = 0x0003;
    info.subvendor_id = 0x5853;
    info.subdevice_id = 0x0003;
    info.revision = 0x01;
    info.class = 0x01;
    info.subclass = 0x00;
    info.prog_if = 0x00;
    info.header_type = 0;
    info.command = PCI_COMMAND_IO;
    info.interrupt_pin = 1;

    rc = pci_device_register(xch, domid, ioservid, &info);
    if (rc < 0)
        goto fail1;

    device_io_state.index = 0;
    device_io_state.order = 8;

    rc = pci_bar_register(device_io_state.index,
                          PCI_BASE_ADDRESS_SPACE_IO,
                          device_io_state.order,
                          &device_io_ops,
                          &device_io_state);
    if (rc < 0)
        goto fail2;

    device_memory_state.index = 1;
    device_memory_state.order = 24;

    rc = pci_bar_register(device_memory_state.index,
                          PCI_BASE_ADDRESS_SPACE_MEMORY |
                          PCI_BASE_ADDRESS_MEM_PREFETCH,
                          device_memory_state.order,
                          &device_memory_ops,
                          &device_memory_state);
    if (rc < 0)
        goto fail3;

    return 0;

fail3:
    DBG("fail3\n");

    pci_bar_deregister(0);

fail2:
    DBG("fail2\n");

    pci_device_deregister();

fail1:
    DBG("fail1\n");

    warn("fail");
    return -1;
}

void
device_teardown(void)
{
    pci_bar_deregister(1);
    pci_bar_deregister(0);
    pci_device_deregister();
}

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
#include <string.h>
#include <assert.h>
#include <inttypes.h>

#include <sys/types.h>

#include <xenctrl.h>

#include "debug.h"
#include "pci.h"
#include "demu.h"

#define FALSE 0

typedef struct pci_bar {
    const io_ops_t *ops;
    int             is_mmio;
    uint64_t        addr;
    uint64_t        size;
    void            (*enable)(void *, uint64_t);
    void            (*disable)(void *);
    void            *priv;
} pci_bar_t;

typedef struct pci {
    uint8_t     bus;
    uint8_t     device:5;
    uint8_t     function:3;
    uint8_t     config[PCI_CONFIG_SIZE];
    uint8_t     mask[PCI_CONFIG_SIZE];
    pci_bar_t   bar[PCI_NUM_BAR];
    uint32_t    irq_pin;
    uint32_t    irq_state;
} pci_t;

static pci_t    pci_state;

static void
pci_bar_enable(unsigned int index)
{
    pci_bar_t	*bar = &pci_state.bar[index];

    assert(bar->enable != NULL);
    bar->enable(bar->priv, bar->addr);
}

static void
pci_bar_disable(unsigned int index)
{
    pci_bar_t	*bar = &pci_state.bar[index];

    assert(bar->disable != NULL);
    bar->disable(bar->priv);
}

static void
pci_bar_update(unsigned int index)
{
    pci_bar_t	*bar = &pci_state.bar[index];
    uint16_t 	cmd = *(uint16_t *)&pci_state.config[PCI_COMMAND];
    uint8_t     offset;
    uint32_t 	addr;
    uint32_t 	mask;

    if (bar->size == 0)
        return;

    if (index == PCI_ROM_SLOT)
	    offset = PCI_ROM_ADDRESS;
    else
	    offset = PCI_BASE_ADDRESS_0 + (index * 4);

    addr = *(uint32_t *)&pci_state.config[offset];
    mask = *(uint32_t *)&pci_state.mask[offset];

    if (index == PCI_ROM_SLOT) {
	    addr &= PCI_ROM_ADDRESS_MASK;
	    mask &= PCI_ROM_ADDRESS_MASK;
    } else {
	    if (bar->is_mmio) {
		    addr &= PCI_BASE_ADDRESS_MEM_MASK;
		    mask &= PCI_BASE_ADDRESS_MEM_MASK;
	    } else {
		    addr &= PCI_BASE_ADDRESS_IO_MASK;
		    mask &= PCI_BASE_ADDRESS_IO_MASK;
	    }
    }

    if ((!(cmd & PCI_COMMAND_IO) && !bar->is_mmio)
        || (!(cmd & PCI_COMMAND_MEMORY) && bar->is_mmio))
	    addr = PCI_BAR_UNMAPPED;

    if (addr == 0 || addr == mask)
        addr = PCI_BAR_UNMAPPED;

    if (bar->addr == addr)
        return;

    if (bar->addr != PCI_BAR_UNMAPPED) {
        pci_bar_disable(index);
        bar->addr = PCI_BAR_UNMAPPED;
    }

    if (addr != PCI_BAR_UNMAPPED) {
	    assert(bar->addr == PCI_BAR_UNMAPPED);
        bar->addr = addr;
        pci_bar_enable(index);
    }
}

static void
pci_config_update()
{
    int	i;

    for (i = 0; i < PCI_NUM_BAR; i++)
        pci_bar_update(i);
}

uint8_t
pci_config_readb(void *priv, uint64_t addr)
{
    uint8_t val;

    assert(priv == NULL);
    val = pci_state.config[addr];
 
    return val;
}

void
pci_config_writeb(void *priv, uint64_t addr, uint8_t val)
{
    uint8_t	mask = pci_state.mask[addr];

    assert(priv == NULL);
    pci_state.config[addr] &= ~mask;
    pci_state.config[addr] |= val & mask;

    pci_config_update();
}

static io_ops_t pci_config_ops = {
    .readb = pci_config_readb,
    .writeb = pci_config_writeb
};

void
pci_device_dump(void)
{
    int	i;

    fprintf(stderr, "      VALUE      |  MASK        \n");
    fprintf(stderr, "      3  2  1  0 |  3  2  1  0  \n");
    fprintf(stderr, "-----------------|--------------\n");

    for (i = 0; i < PCI_CONFIG_HEADER_SIZE; i += 4) {
        fprintf(stderr, "%02x | %02x %02x %02x %02x | %02x %02x %02x %02x\n",
                i,
                pci_state.config[i + 3],
                pci_state.config[i + 2],
                pci_state.config[i + 1],
                pci_state.config[i ],
                pci_state.mask[i + 3],
                pci_state.mask[i + 2],
                pci_state.mask[i + 1],
                pci_state.mask[i ]);
    }
}

int
pci_device_register(const pci_info_t *info)
{
    int	rc;

    if (info->bus & ~0xff ||
        info->device & ~0x1f ||
        info->function & ~0x07)
        goto fail1;

    pci_state.bus = info->bus;
    pci_state.device = info->device;
    pci_state.function = info->function;

    *(uint16_t *)&pci_state.config[PCI_VENDOR_ID] = info->vendor_id;
    *(uint16_t *)&pci_state.config[PCI_DEVICE_ID] = info->device_id;
    pci_state.config[PCI_REVISION_ID] = info->revision;
    pci_state.config[PCI_CLASS_PROG] = info->prog_if;
    pci_state.config[PCI_CLASS_DEVICE + 1] = info->class;
    pci_state.config[PCI_CLASS_DEVICE] = info->subclass;
    pci_state.config[PCI_HEADER_TYPE] = info->header_type;
    *(uint16_t *)&pci_state.config[PCI_SUBSYSTEM_VENDOR_ID] = info->subvendor_id;
    *(uint16_t *)&pci_state.config[PCI_SUBSYSTEM_ID] = info->subdevice_id;
    *(uint16_t *)&pci_state.config[PCI_COMMAND] = info->command;
    pci_state.config[PCI_INTERRUPT_PIN] = info->interrupt_pin;

    pci_state.mask[PCI_CACHE_LINE_SIZE] = 0xff;
    pci_state.mask[PCI_INTERRUPT_LINE] = 0xff;
    *(uint16_t *)&pci_state.mask[PCI_COMMAND] = PCI_COMMAND_IO |
                                          PCI_COMMAND_MEMORY |
                                          PCI_COMMAND_MASTER |
                                          PCI_COMMAND_INTX_DISABLE;
    memset(&pci_state.mask[PCI_CONFIG_HEADER_SIZE], 0xff,
           PCI_CONFIG_SIZE - PCI_CONFIG_HEADER_SIZE);

    rc = demu_register_pci_config_space(pci_state.bus, pci_state.device, pci_state.function,
                                        &pci_config_ops, NULL);
    if (rc < 0)
        goto fail2;

    return 0;

fail2:
    DBG("fail2\n");

fail1:
    DBG("fail1\n");

    warn("fail");
    return -1;
}

void
pci_device_deregister(void)
{
    demu_deregister_pci_config_space(pci_state.bus, pci_state.device, pci_state.function);
}

int
pci_bar_register(unsigned int index, uint8_t type, uint64_t size,
                 void (*enable)(void *priv, uint64_t addr),
                 void (*disable)(void *priv),
                 void *priv)
{
    pci_bar_t	*bar;
    uint32_t    mask;
    uint8_t     offset;

    DBG("%d: %"PRIx64"\n", index, size);

    if (index >= PCI_NUM_BAR)
        goto fail1;

    bar = &pci_state.bar[index];

    if (enable == NULL ||
        disable == NULL)
        goto fail2;

    bar->priv = priv;
    bar->enable = enable;
    bar->disable = disable;
    bar->is_mmio = !(type & PCI_BASE_ADDRESS_SPACE_IO);
    bar->addr = PCI_BAR_UNMAPPED;
    bar->size = size;

    mask = ~(bar->size - 1);

    if (index == PCI_ROM_SLOT) {
	    offset = PCI_ROM_ADDRESS;
	    mask |= PCI_ROM_ADDRESS_ENABLE;
    } else {
	    offset = PCI_BASE_ADDRESS_0 + (index * 4);
    }

    *(uint32_t *)&pci_state.config[offset] = type;
    *(uint32_t *)&pci_state.mask[offset] = mask;

    return 0;

fail2:
    DBG("fail2\n");

fail1:
    DBG("fail1\n");

    warn("fail");
    return -1;
}

void
pci_bar_deregister(unsigned int index)
{
    pci_bar_t	*bar = &pci_state.bar[index];

    DBG("%d\n", index);

    if (bar->addr == PCI_BAR_UNMAPPED)
        return;

    assert(bar->disable != NULL);
    bar->disable(bar->priv);
    bar->size = 0;
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


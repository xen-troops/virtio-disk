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
#include <assert.h>

#include <sys/types.h>

#include <xenctrl.h>

#include "debug.h"
#include "pci.h"

#define FALSE 0

typedef struct pci_bar {
    const bar_ops_t *ops;
    int             is_mmio;
    int             enable;
    uint32_t        addr;
    uint32_t        size;
    void            *priv;
} pci_bar_t;

typedef struct pci {
    xc_interface    *xch;
    domid_t         domid;
    ioservid_t      ioservid;
    uint16_t        bdf;
    uint8_t         config[PCI_CONFIG_SIZE];
    uint8_t         mask[PCI_CONFIG_SIZE];
    pci_bar_t       bar[PCI_NUM_BAR];
    uint32_t        irq_pin;
    uint32_t        irq_state;
} pci_t;

static pci_t    pci;

int
pci_device_register(xc_interface *xch, domid_t domid, ioservid_t ioservid,
                    const pci_info_t *info)
{
    int     rc;

    pci.xch = xch;
    pci.domid = domid;
    pci.ioservid = ioservid;

    if (info->bus & ~0xff ||
        info->device & ~0x1f ||
        info->function & ~0x07)
        goto fail1;

    pci.bdf = (info->bus << 8) | (info->device << 3) | (info->function);

    *(uint16_t *)&pci.config[PCI_VENDOR_ID] = info->vendor_id;
    *(uint16_t *)&pci.config[PCI_DEVICE_ID] = info->device_id;
    pci.config[PCI_REVISION_ID] = info->revision;
    pci.config[PCI_CLASS_PROG] = info->prog_if;
    pci.config[PCI_CLASS_DEVICE + 1] = info->class;
    pci.config[PCI_CLASS_DEVICE] = info->subclass;
    pci.config[PCI_HEADER_TYPE] = info->header_type;
    *(uint16_t *)&pci.config[PCI_SUBSYSTEM_VENDOR_ID] = info->subvendor_id;
    *(uint16_t *)&pci.config[PCI_SUBSYSTEM_ID] = info->subdevice_id;
    *(uint16_t *)&pci.config[PCI_COMMAND] = info->command;
    pci.config[PCI_INTERRUPT_PIN] = info->interrupt_pin;

    pci.mask[PCI_CACHE_LINE_SIZE] = 0xff;
    pci.mask[PCI_INTERRUPT_LINE] = 0xff;
    *(uint16_t *)&pci.mask[PCI_COMMAND] = PCI_COMMAND_IO |
                                          PCI_COMMAND_MEMORY |
                                          PCI_COMMAND_MASTER |
                                          PCI_COMMAND_INTX_DISABLE;
    memset(&pci.mask[PCI_CONFIG_HEADER_SIZE], 0xff,
           PCI_CONFIG_SIZE - PCI_CONFIG_HEADER_SIZE);

    DBG("%02x:%02x:%02x\n", info->bus, info->device, info->function);

    rc = xc_hvm_map_pcidev_to_ioreq_server(xch, domid, ioservid, pci.bdf);
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
    DBG("%02x:%02x:%02x\n",
        (pci.bdf >> 8) & 0xff,
        (pci.bdf >> 5) & 0x1f,
        (pci.bdf     ) & 0x07);

    (void) xc_hvm_unmap_pcidev_from_ioreq_server(pci.xch, pci.domid,
						 pci.ioservid, pci.bdf);
}

int
pci_bar_register(unsigned int index, uint8_t type, unsigned int order,
                 const bar_ops_t *ops, void *priv)
{
    pci_bar_t   *bar;

    DBG("%d: %08x\n", index, 1u << order);

    if (index >= PCI_NUM_BAR)
        goto fail;

    bar = &pci.bar[index];

    if (bar->enable)
        goto fail;

    if (ops == NULL ||
        ops->readb == NULL ||
        ops->writeb == NULL)
        goto fail;

    bar->ops = ops;
    bar->is_mmio = !(type & PCI_BASE_ADDRESS_SPACE_IO);
    bar->size = 1u << order;

    *(uint32_t *)&pci.config[PCI_BASE_ADDRESS_0 + (index * 4)] = type;
    *(uint32_t *)&pci.mask[PCI_BASE_ADDRESS_0 + (index * 4)] = ~(bar->size - 1);

    bar->enable = 1;
    bar->addr = PCI_BAR_UNMAPPED;

    bar->priv = priv;

    return 0;

fail:
    return -1;
}

void
pci_bar_deregister(unsigned int index)
{
    pci_bar_t *bar = &pci.bar[index];

    DBG("%d\n", index);

    if (bar->addr == PCI_BAR_UNMAPPED)
        return;

    (void) xc_hvm_unmap_io_range_from_ioreq_server(pci.xch,
						   pci.domid,
						   pci.ioservid,
						   bar->is_mmio,
						   bar->addr);
}

static pci_bar_t *
pci_bar_get(int is_mmio, uint64_t addr)
{
    int i;

    for (i = 0; i < PCI_NUM_BAR; i++)
    {
        pci_bar_t *bar = &pci.bar[i];

        if (!bar->enable || bar->is_mmio != is_mmio)
            continue;

        if (bar->addr <= addr && addr < (bar->addr + bar->size))
            return bar;
    }

    return NULL;
}

#define PCI_BAR_READ(_fn, _priv, _addr, _size, _count, _val)    \
do {                                                            \
    int             _i = 0;                                     \
    unsigned int    _shift = 0;                                 \
                                                                \
    (_val) = 0;                                                 \
    for (_i = 0; _i < (_count); _i++)                           \
    {                                                           \
        (_val) |= (_fn)((_priv), (_addr)) << _shift;            \
        _shift += 8 * (_size);                                  \
        (_addr) += (_size);                                     \
    }                                                           \
} while (FALSE)

uint32_t
pci_bar_read(int is_mmio, uint64_t addr, uint64_t size)
{
    pci_bar_t   *bar = pci_bar_get(is_mmio, addr);
    uint32_t    val = 0;

    assert(bar != NULL);

    addr -= bar->addr;

    switch (size) {
    case 1:
        val = bar->ops->readb(bar->priv, addr);
        break;

    case 2:
        if (bar->ops->readw == NULL)
            PCI_BAR_READ(bar->ops->readb, bar->priv, addr, 1, 2, val);
        else
            val = bar->ops->readw(bar->priv, addr);
        break;

    case 4:
        if (bar->ops->readl == NULL) {
            if (bar->ops->readw == NULL)
                PCI_BAR_READ(bar->ops->readb, bar->priv, addr, 1, 4, val);
            else
                PCI_BAR_READ(bar->ops->readw, bar->priv, addr, 2, 2, val);
        } else {
            val = bar->ops->readl(bar->priv, addr);
        }
        break;

    default:
        assert(FALSE);
    }

    return val;
}

#define PCI_BAR_WRITE(_fn, _priv, _addr, _size, _count, _val)   \
do {                                                            \
    int             _i = 0;                                     \
    unsigned int    _shift = 0;                                 \
                                                                \
    (_val) = 0;                                                 \
    for (_i = 0; _i < (_count); _i++)                           \
    {                                                           \
        (_fn)((_priv), (_addr), (_val) >> _shift);              \
        _shift += 8 * (_size);                                  \
        (_addr) += (_size);                                     \
    }                                                           \
} while (FALSE)

void
pci_bar_write(int is_mmio, uint64_t addr, uint64_t size, uint32_t val)
{
    pci_bar_t   *bar = pci_bar_get(is_mmio, addr);

    assert(bar != NULL);

    addr -= bar->addr;

    switch (size) {
    case 1:
        bar->ops->writeb(bar->priv, addr, val);
        break;

    case 2:
        if (bar->ops->writew == NULL)
            PCI_BAR_WRITE(bar->ops->writeb, bar->priv, addr, 1, 2, val);
        else
            bar->ops->writew(bar->priv, addr, val);
        break;

    case 4:
        if (bar->ops->writel == NULL) {
            if (bar->ops->writew == NULL)
                PCI_BAR_WRITE(bar->ops->writeb, bar->priv, addr, 1, 4, val);
            else
                PCI_BAR_WRITE(bar->ops->writew, bar->priv, addr, 2, 2, val);
        } else {
            bar->ops->writel(bar->priv, addr, val);
        }
        break;

    default:
        assert(FALSE);
    }
}

static void
pci_map_bar(unsigned int index)
{
    pci_bar_t *bar = &pci.bar[index];

    if (bar->ops->map)
        bar->ops->map(bar->priv, bar->addr);

    (void) xc_hvm_map_io_range_to_ioreq_server(pci.xch,
                                               pci.domid,
                                               pci.ioservid,
                                               bar->is_mmio,
                                               bar->addr,
                                               bar->addr + bar->size - 1);
}

static void
pci_unmap_bar(unsigned int index)
{
    pci_bar_t *bar = &pci.bar[index];

    DBG("%d\n", index);

    (void) xc_hvm_unmap_io_range_from_ioreq_server(pci.xch,
                                                   pci.domid,
                                                   pci.ioservid,
                                                   bar->is_mmio,
                                                   bar->addr);

    if (bar->ops->unmap)
        bar->ops->unmap(bar->priv);
}

static void
pci_update_bar(unsigned int index)
{
    pci_bar_t *bar = &pci.bar[index];
    uint32_t addr = *(uint32_t *)&pci.config[PCI_BASE_ADDRESS_0 + (index * 4)];
    uint16_t cmd = *(uint16_t *)&pci.config[PCI_COMMAND];
    uint32_t mask = ~(bar->size - 1);

    if (!bar->enable)
        return;

    if (bar->is_mmio)
        addr &= PCI_BASE_ADDRESS_MEM_MASK;
    else
        addr &= PCI_BASE_ADDRESS_IO_MASK;

    if ((!(cmd & PCI_COMMAND_IO) && !bar->is_mmio)
        || (!(cmd & PCI_COMMAND_MEMORY) && bar->is_mmio))
        addr = PCI_BAR_UNMAPPED;

    if (addr == 0 || addr == mask)
        addr = PCI_BAR_UNMAPPED;

    if (bar->addr == addr)
        return;

    if (bar->addr != PCI_BAR_UNMAPPED) {
        pci_unmap_bar(index);
        bar->addr = PCI_BAR_UNMAPPED;
    }

    if (addr != PCI_BAR_UNMAPPED) {
        bar->addr = addr;
        pci_map_bar(index);
    }
}

static void
pci_update_config()
{
    int i;

    for (i = 0; i < PCI_NUM_BAR; i++)
        pci_update_bar(i);
}

uint32_t
pci_config_read(uint64_t addr, uint64_t size)
{
    uint64_t    i;
    uint32_t    val;
    uint16_t    bdf;

    val = ~(0u);

    if (!(addr & (1ull << 31)))
        goto done;

    bdf = (uint16_t)(addr >> 8);

    if (bdf != pci.bdf)
        goto done;

    addr &= 0xff;
    addr += size >> 16;
    size &= 0xffff;

    val = 0;
    for (i = 0; i < size; i++) {
        if ((addr + i) < PCI_CONFIG_SIZE)
            val |= (uint32_t)pci.config[addr + i] << (i * 8);
        else
            val |= (uint32_t)0xff << (i + 8);
    }

    DBG("%02x (%02x) -> %08x\n",
        (uint8_t)addr,
        (uint8_t)size,
        val);

done:
    return val;
}

void
pci_config_write(uint64_t addr, uint64_t size, uint32_t val)
{
    uint64_t    i;
    uint8_t     mask;
    uint16_t    bdf;

    if (!(addr & (1ull << 31)))
        return;

    bdf = (uint16_t)(addr >> 8);

    if (bdf != pci.bdf)
        return;

    addr &= 0xff;
    addr += size >> 16;
    size &= 0xffff;

    DBG("%02x (%02x) <- %08x\n",
        (uint8_t)addr,
        (uint8_t)size,
        val);

    for (i = 0; i < size; i++) {
        if ((addr + i) < PCI_CONFIG_SIZE) {
            mask = pci.mask[addr + i];
            pci.config[addr + i] &= ~mask;
            pci.config[addr + i] |= (uint8_t)(val >> (i * 8)) & mask;
        }
    }

    pci_update_config();
}

void
pci_config_dump(void)
{
    int i;

    fprintf(stderr, "    3  2  1  0\n");
    fprintf(stderr, "--------------\n");

    for (i = 0; i < PCI_CONFIG_HEADER_SIZE; i += 4) {
        fprintf(stderr, "%02x |%02x %02x %02x %02x\n",
                i,
                pci.config[i + 3],
                pci.config[i + 2],
                pci.config[i + 1],
                pci.config[i ]);
    }
}

/*  
 * Copyright (c) 2012, Citrix Systems Inc.
 * Copyright (C) 2024 EPAM Systems Inc.
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
#include <assert.h>
#include "debug.h"
#include <demu.h>
#include <err.h>
#include <errno.h>
#include "pci.h"
#include <string.h>

#define FALSE 0

typedef struct pci_bar {
    const bar_ops_t *ops;
    int is_mmio;
    int enable;
    uint32_t addr;
    uint32_t size;
    void *priv;
} pci_bar_t;

typedef struct pci {
    uint16_t bdf;
    uint8_t config[PCI_CONFIG_SIZE];
    uint8_t mask[PCI_CONFIG_SIZE];
    pci_bar_t bar[PCI_NUM_BAR];
} pci_t;

static pci_t pci;
static void pci_update_bar(unsigned int index);

int pci_device_register(const pci_info_t *info)
{
    if (info->bus & ~0xff ||
        info->device & ~0x1f ||
        info->function & ~0x07)
    {
        warnx("bad BDF");
        return -EINVAL;
    }

    pci.bdf = (info->bus << 8) | (info->device << 3) | (info->function);

    *(uint16_t *)&pci.config[PCI_VENDOR_ID] = info->vendor_id;
    *(uint16_t *)&pci.config[PCI_DEVICE_ID] = info->device_id;
    *(uint16_t *)&pci.config[PCI_STATUS] = info->status;
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
    pci.mask[PCI_INTERRUPT_LINE] = info->interrupt_line;
    *(uint16_t *)&pci.mask[PCI_COMMAND] = PCI_COMMAND_IO |
                                          PCI_COMMAND_MEMORY |
                                          PCI_COMMAND_MASTER |
                                          PCI_COMMAND_INTX_DISABLE;
    memset(&pci.mask[PCI_CONFIG_HEADER_SIZE], 0xff,
           PCI_CONFIG_SIZE - PCI_CONFIG_HEADER_SIZE);

    pci.config[PCI_CAPABILITY_LIST] = PCI_CAP_OFF;
    memcpy(&pci.config[PCI_CAP_OFF], &info->virtio, sizeof(struct virtio_caps));

    return demu_register_pcidev(info->bus, info->device, info->function);
}

void
pci_device_deregister(void)
{
    uint8_t bus = (pci.bdf >> 8) & 0xff;
    uint8_t device = (pci.bdf >> 3) & 0x1f;
    uint8_t function = pci.bdf & 0x07;

    demu_deregister_pcidev(bus, device, function);
 }

int
pci_bar_register(unsigned int index, uint8_t type, unsigned int order,
                 const bar_ops_t *ops, void *priv)
{
    pci_bar_t *bar;

    DBG("%d: %08x\n", index, 1u << order);

    if (index >= PCI_NUM_BAR) {
        warnx("unsupported BAR index %d", index);
        return -EINVAL;
    }

    bar = &pci.bar[index];

    if (bar->enable) {
        warnx("BAR%d busy", index);
        return -EBUSY;
    }

    if (ops == NULL ||
        ops->readb == NULL ||
        ops->writeb == NULL)
    {
        return -EFAULT;
    }

    bar->ops = ops;
    bar->is_mmio = !(type & PCI_BASE_ADDRESS_SPACE_IO);
    bar->size = 1u << order;

    *(uint32_t *)&pci.config[PCI_BASE_ADDRESS_0 + (index * 4)] = type;
    *(uint32_t *)&pci.mask[PCI_BASE_ADDRESS_0 + (index * 4)] =
        ~(bar->size - 1);

    bar->enable = 1;
    bar->addr = PCI_BAR_UNMAPPED;

    bar->priv = priv;

    return 0;
}

void
pci_bar_deregister(unsigned int index)
{
    pci_bar_t *bar = &pci.bar[index];

    DBG("%d\n", index);

    bar->addr = PCI_BAR_UNMAPPED;
    bar->priv = NULL;
    bar->enable = 0;
    bar->ops = NULL;
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

static uint32_t
pci_bar_read(int is_mmio, uint64_t addr, uint64_t size)
{
    pci_bar_t *bar = pci_bar_get(is_mmio, addr);
    uint32_t val = 0;

    assert(bar != NULL);
    if (!bar)
       return 0;

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
    int _i = 0;                                                 \
    unsigned int _shift = 0;                                    \
                                                                \
    (_val) = 0;                                                 \
    for (_i = 0; _i < (_count); _i++)                           \
    {                                                           \
        (_fn)((_priv), (_addr), (_val) >> _shift);              \
        _shift += 8 * (_size);                                  \
        (_addr) += (_size);                                     \
    }                                                           \
} while (FALSE)

static void
pci_bar_write(int is_mmio, uint64_t addr, uint64_t size, uint32_t val)
{
    pci_bar_t *bar = pci_bar_get(is_mmio, addr);

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

static void pci_mmio_callback(u64 addr, u8 *data,
                              u32 len, u8 is_write, void *ptr)
{
    pci_bar_t *bar = ptr;

/*    DBG("%s %llx size=%u\n", is_write ? "write" : "read", addr, len); */
    if (is_write)
        pci_bar_write(bar->is_mmio, addr, len, *((uint32_t*)data));
    else
       *((uint32_t*)data) = pci_bar_read(bar->is_mmio, addr, len);
}

static void
pci_map_bar(unsigned int index)
{
    pci_bar_t *bar = &pci.bar[index];

    DBG("%d: %08x\n", index, bar->addr);

    if (bar->ops->map)
        bar->ops->map(bar->priv, bar->addr);

    demu_register_memory_space(bar->addr, bar->size,
                               pci_mmio_callback, bar);
}

static void
pci_unmap_bar(unsigned int index)
{
    pci_bar_t *bar = &pci.bar[index];

    DBG("%d\n", index);

    (void) demu_deregister_memory_space(bar->addr);

    if (bar->ops->unmap)
        bar->ops->unmap(bar->priv);
}

static void
pci_update_bar(unsigned int index)
{
    pci_bar_t *bar = &pci.bar[index];
    uint32_t addr = *(uint32_t *)&pci.config[PCI_BASE_ADDRESS_0 +
                                             (index * 4)];
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

void
pci_update_config()
{
    int i;

    for (i = 0; i < PCI_NUM_BAR; i++)
        pci_update_bar(i);
}

uint32_t
pci_config_read(uint64_t offset, uint64_t size)
{
    uint64_t i;
    uint32_t val = 0;

    offset &= 0xff;

    for (i = 0; i < size; i++) {
        if ((offset + i) < PCI_CONFIG_SIZE)
            val |= (uint32_t)pci.config[offset + i] << (i * 8);
        else
            val |= (uint32_t)0xff << (i + 8);
    }

    return val;
}

void
pci_config_write(uint64_t offset, uint64_t size, uint32_t val)
{
    uint64_t i;
    uint8_t mask;

    offset &= 0xff;
    offset += size >> 16;
    size &= 0xffff;

    for (i = 0; i < size; i++) {
        if ((offset + i) < PCI_CONFIG_SIZE) {
            mask = pci.mask[offset + i];
            pci.config[offset + i] &= ~mask;
            pci.config[offset + i] |= (uint8_t)(val >> (i * 8)) & mask;

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

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * c-tab-always-indent: nil
 * indent-tabs-mode: nil
 * End:
 */

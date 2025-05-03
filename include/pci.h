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
#ifndef  _PCI_H
#define  _PCI_H

#include <linux/pci_regs.h>
#include <kvm/pci.h>
#include <xenctrl.h>
#include <xendevicemodel.h>

#define PCI_VENDOR_ID_REDHAT_QUMRANET		0x1af4 
#define PCI_SUBSYSTEM_VENDOR_ID_REDHAT_QUMRANET 0x1af4
#define PCI_DEVICE_ID_VIRTIO_BASE          0x1040
#define PCI_SUBSYS_ID_VIRTIO_BASE          0x0040
#define PCI_HEADER_TYPE_NORMAL			0

#define PCI_NUM_BAR             6
#define PCI_CONFIG_HEADER_SIZE  0x40
#define PCI_CONFIG_SIZE         0x100
#define PCI_BAR_UNMAPPED        (~(0u))
#define PCI_CAP_OFF		PCI_CONFIG_HEADER_SIZE

typedef struct pci_info {
        uint8_t bus;
        uint8_t device:5;
        uint8_t function:3;
        uint16_t vendor_id;
        uint16_t device_id;
        uint16_t command;
        uint16_t status;
        uint16_t subvendor_id;
        uint16_t subdevice_id;
        uint8_t revision;
        int class;
        uint8_t subclass;
        uint8_t prog_if;
        uint8_t header_type;
        uint8_t interrupt_pin;
        uint8_t interrupt_line;
        struct virtio_caps virtio;
} pci_info_t;

int pci_device_register(const pci_info_t *info);
void pci_device_deregister(void);

typedef struct bar_ops {
        void (*map)(void *priv, uint64_t addr);
        void (*unmap)(void *priv);
        uint8_t (*readb)(void *priv, uint64_t offset);
        uint16_t (*readw)(void *priv, uint64_t offset);
        uint32_t (*readl)(void *priv, uint64_t offset);
        void (*writeb)(void *priv, uint64_t offset, uint8_t val);
        void (*writew)(void *priv, uint64_t offset, uint16_t val);
        void (*writel)(void *priv, uint64_t offset, uint32_t val);
} bar_ops_t;

int pci_bar_register(unsigned int index, uint8_t type, unsigned int order,
                     const bar_ops_t *ops, void *priv);
void pci_bar_deregister(unsigned int index);
uint32_t pci_config_read(uint64_t offset, uint64_t size);
void pci_config_write(uint64_t offset, uint64_t size, uint32_t val);
void pci_config_dump();

#endif  /* _PCI_H */


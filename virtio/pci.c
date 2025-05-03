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
#include <debug.h>
#include <demu.h>
#include <err.h>
#include <linux/byteorder.h>
#include <pci.h>
#include <virtio-pci.h>

#define VPCI_CFG_COMMON_SIZE	sizeof(struct virtio_pci_common_cfg)
#define VPCI_CFG_COMMON_START	0
#define VPCI_CFG_COMMON_END	(VPCI_CFG_COMMON_SIZE - 1)
/*
 * Use a naturally aligned 4-byte doorbell, in case we ever want to
 * implement VIRTIO_F_NOTIFICATION_DATA
 */
#define VPCI_CFG_NOTIFY_SIZE	4
#define VPCI_CFG_NOTIFY_START	(VPCI_CFG_COMMON_END + 1)
#define VPCI_CFG_NOTIFY_END	(VPCI_CFG_COMMON_END + VPCI_CFG_NOTIFY_SIZE)
#define VPCI_CFG_ISR_SIZE	4
#define VPCI_CFG_ISR_START	(VPCI_CFG_NOTIFY_END + 1)
#define VPCI_CFG_ISR_END	(VPCI_CFG_NOTIFY_END + VPCI_CFG_ISR_SIZE)
/*
 * We're at 64 bytes. Use the remaining 192 bytes in PCI_IO_SIZE for the
 * device-specific config space. It's sufficient for the devices we
 * currently implement (virtio_blk_config is 60 bytes) and, I think, all
 * existing virtio 1.2 devices.
 */
#define VPCI_CFG_DEV_START	(VPCI_CFG_ISR_END + 1)
#define VPCI_CFG_DEV_END	((PCI_IO_SIZE) - 1)
#define VPCI_CFG_DEV_SIZE	(VPCI_CFG_DEV_END - VPCI_CFG_DEV_START + 1)

static uint8_t
device_memory_readb(void *priv, uint64_t offset)
{
    uint32_t res;

    virtio_pci_modern__io_mmio_callback(NULL, offset, (uint8_t *)&res, 1,
                                        false, priv);
    /* DBG("off=%lx res=%x\n", offset, res); */
    return (uint8_t)res;
}

static uint16_t
device_memory_readw(void *priv, uint64_t offset)
{
    uint32_t res;

    virtio_pci_modern__io_mmio_callback(NULL, offset, (uint8_t *)&res, 2,
                                        false, priv);
    /* DBG("off=%lx res=%x\n", offset, res); */
    return (uint16_t)res;
}

static uint32_t
device_memory_readl(void *priv, uint64_t offset)
{
    uint32_t res;

    virtio_pci_modern__io_mmio_callback(NULL, offset, (uint8_t *)&res, 4,
                                        false, priv);
    /* DBG("off=%lx res=%x\n", offset, res); */
    return res;
}

static void
device_memory_writeb(void *priv, uint64_t offset, uint8_t val)
{
    struct kvm_cpu vcpu;

    /* DBG("off=%lx val=0x%x\n", offset, val); */
    virtio_pci_modern__io_mmio_callback(&vcpu, offset, &val, 1,
                                        true, priv);
}

void device_memory_writew(void *priv, uint64_t offset, uint16_t val)
{
    struct kvm_cpu vcpu;

    /* DBG("off=%lx val=0x%x\n", offset, val); */
    virtio_pci_modern__io_mmio_callback(&vcpu, offset, (uint8_t *)&val, 2,
                                        true, priv);
}

void device_memory_writel(void *priv, uint64_t offset, uint32_t val)
{
    struct kvm_cpu vcpu;

    /* DBG("off=%lx val=0x%x\n", offset, val); */
    virtio_pci_modern__io_mmio_callback(&vcpu, offset, (uint8_t *)&val, 4,
                                        true, priv);
}

static bar_ops_t device_memory_ops = {
    .readb = device_memory_readb,
    .readw = device_memory_readw,
    .readl = device_memory_readl,
    .writeb = device_memory_writeb,
    .writew = device_memory_writew,
    .writel = device_memory_writel,
};

static void pci_config_mmio_callback(u64 addr, u8 *data,
				u32 len, u8 is_write, void *ptr)
{
    struct virtio_pci *vpci = ptr;
    uint64_t offset = addr - vpci->cfg_base;

    /* DBG("%s %llx size=%u\n", is_write ? "write" : "read", addr, len); */
    if (is_write)
        pci_config_write(offset, len, *((uint32_t*)data));
    else
       *((uint32_t*)data) = pci_config_read(offset, len);
}

int virtio_pci_init(struct kvm *kvm, void *dev, struct virtio_device *vdev,
		    int device_id, int subsys_id, int class, u32 addr, u32 irq)
{
    struct virtio_pci *vpci = vdev->virtio;
    size_t next_cap_off = PCI_CAP_OFF;
    pci_info_t info;
    int rc;

    vpci->cfg_base = addr;
    vpci->dev = dev;
    vpci->kvm = kvm;
    vpci->config_vector = VIRTIO_MSI_NO_VECTOR;
    memset(vpci->vq_vector, 0xff, sizeof(vpci->vq_vector));
    vpci->legacy_irq_line = irq;

    /*TODO: shall not be hard-coded */
    info.bus = 0x0;
    info.device = 0x3;
    info.function = 0x0;

    info.vendor_id = cpu_to_le16(PCI_VENDOR_ID_REDHAT_QUMRANET);
    info.device_id = cpu_to_le16(device_id);
    info.subvendor_id = cpu_to_le16(PCI_SUBSYSTEM_VENDOR_ID_REDHAT_QUMRANET);
    info.subdevice_id = cpu_to_le16(subsys_id);
    info.revision = vdev->legacy ? 0x00 : 0x01,
    info.prog_if = class & 0xff;
    info.subclass = (class >> 8) & 0xff;
    info.class = (class >> 16) & 0xff;
    info.header_type = PCI_HEADER_TYPE_NORMAL;
    info.command = PCI_COMMAND_IO | PCI_COMMAND_MEMORY;
    info.status = cpu_to_le16(PCI_STATUS_CAP_LIST);
    info.interrupt_pin = 1;
    info.interrupt_line = irq;

    info.device_id = cpu_to_le16(PCI_DEVICE_ID_VIRTIO_BASE + subsys_id);
    info.subdevice_id = cpu_to_le16(PCI_SUBSYS_ID_VIRTIO_BASE + subsys_id);

    next_cap_off += sizeof(info.virtio.common);
    info.virtio.common = (struct virtio_pci_cap) {
        .cap_vndr = PCI_CAP_ID_VNDR,
        .cap_next = next_cap_off,
        .cap_len  = sizeof(info.virtio.common),
        .cfg_type = VIRTIO_PCI_CAP_COMMON_CFG,
        .bar      = 1,
        .offset   = cpu_to_le32(VPCI_CFG_COMMON_START),
        .length   = cpu_to_le32(VPCI_CFG_COMMON_SIZE),
	};

    next_cap_off += sizeof(info.virtio.notify);
    info.virtio.notify = (struct virtio_pci_notify_cap) {
        .cap.cap_vndr = PCI_CAP_ID_VNDR,
        .cap.cap_next = next_cap_off,
        .cap.cap_len  = sizeof(info.virtio.notify),
        .cap.cfg_type = VIRTIO_PCI_CAP_NOTIFY_CFG,
        .cap.bar      = 1,
        .cap.offset   = cpu_to_le32(VPCI_CFG_NOTIFY_START),
        .cap.length   = cpu_to_le32(VPCI_CFG_NOTIFY_SIZE),
    };

    next_cap_off += sizeof(info.virtio.isr);
    info.virtio.isr = (struct virtio_pci_cap) {
        .cap_vndr = PCI_CAP_ID_VNDR,
        .cap_next = next_cap_off,
        .cap_len  = sizeof(info.virtio.isr),
        .cfg_type = VIRTIO_PCI_CAP_ISR_CFG,
        .bar      = 1,
        .offset   = cpu_to_le32(VPCI_CFG_ISR_START),
        .length   = cpu_to_le32(VPCI_CFG_ISR_SIZE),
    };

    next_cap_off += sizeof(info.virtio.device);
    info.virtio.device = (struct virtio_pci_cap) {
        .cap_vndr = PCI_CAP_ID_VNDR,
        .cap_next = next_cap_off,
        .cap_len  = sizeof(info.virtio.device),
        .cfg_type = VIRTIO_PCI_CAP_DEVICE_CFG,
        .bar      = 1,
        .offset   = cpu_to_le32(VPCI_CFG_DEV_START),
        .length   = cpu_to_le32(VPCI_CFG_DEV_SIZE),
    };

    info.virtio.pci = (struct virtio_pci_cfg_cap) {
        .cap.cap_vndr = PCI_CAP_ID_VNDR,
        .cap.cap_next = 0,
        .cap.cap_len  = sizeof(info.virtio.pci),
        .cap.cfg_type = VIRTIO_PCI_CAP_PCI_CFG,
    };

    rc = pci_device_register(&info);
    if (rc < 0)
        goto fail1;

    rc = pci_bar_register(1,
                          PCI_BASE_ADDRESS_SPACE_MEMORY |
                          PCI_BASE_ADDRESS_MEM_PREFETCH,
                          8,
                          &device_memory_ops,
                          vdev);
    if (rc < 0)
        goto fail2;

    rc = demu_register_memory_space(vpci->cfg_base, 0x8000,
                                    pci_config_mmio_callback, vpci);
    if (rc < 0)
        goto fail3;

    return 0;

fail3:
    DBG("fail3\n");

    pci_bar_deregister(1);

fail2:
    DBG("fail2\n");

    pci_device_deregister();

fail1:
    DBG("fail1\n");

    warnx("fail");
    return rc;
}

int virtio_pci_exit(struct kvm *kvm, struct virtio_device *vdev)
{
    struct virtio_pci *vpci = vdev->virtio;

    demu_deregister_memory_space(vpci->cfg_base);
    pci_bar_deregister(1);
    pci_device_deregister();
    return 0;
}

int virtio_pci_reset(struct kvm *kvm, struct virtio_device *vdev)
{
    DBG("not implemented\n");
    return -EFAULT;
}

int virtio_pci_init_vq(struct kvm *kvm, struct virtio_device *vdev, int vq)
{
    struct virtio_pci *vpci = vdev->virtio;
    return vdev->ops->init_vq(kvm, vpci->dev, vq);
}

void virtio_pci_exit_vq(struct kvm *kvm, struct virtio_device *vdev, int vq)
{
    struct virtio_pci *vpci = vdev->virtio;

    vpci->gsis[vq] = 0;
    vpci->vq_vector[vq] = VIRTIO_MSI_NO_VECTOR;
    virtio_exit_vq(kvm, vdev, vpci->dev, vq);
}

int virtio_pci_signal_vq(struct kvm *kvm, struct virtio_device *vdev, u32 vq)
{
    struct virtio_pci *vpci = vdev->virtio;

    vpci->isr = VIRTIO_IRQ_HIGH;
    kvm__irq_line(kvm, vpci->legacy_irq_line, VIRTIO_IRQ_HIGH);
    return 0;
}

int virtio_pci_signal_config(struct kvm *kvm, struct virtio_device *vdev)
{
    DBG("not implemented\n");
    return -EFAULT;
}

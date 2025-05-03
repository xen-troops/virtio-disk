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
#ifndef  _DEVICE_H
#define  _DEVICE_H

#include "demu.h"
#include <err.h>
#include "kvm/virtio.h"
#include <xenctrl.h>
#include <xendevicemodel.h>

#define VIRTIO_PCI_MAX_VQ	32
#define VIRTIO_PCI_MAX_CONFIG	1

#define ALIGN_UP(x, s)		ALIGN((x) + (s) - 1, (s))
#define VIRTIO_NR_MSIX		(VIRTIO_PCI_MAX_VQ + VIRTIO_PCI_MAX_CONFIG)
#define VIRTIO_MSIX_TABLE_SIZE	(VIRTIO_NR_MSIX * 16)
#define VIRTIO_MSIX_PBA_SIZE	(ALIGN_UP(VIRTIO_MSIX_TABLE_SIZE, 64) / 8)
#define VIRTIO_MSIX_BAR_SIZE	(1UL << fls_long(VIRTIO_MSIX_TABLE_SIZE + \
						 VIRTIO_MSIX_PBA_SIZE))
struct kvm;

struct virtio_pci {
/*	struct pci_device_header pci_hdr;*/
/*	struct device_header	dev_hdr;*/
	u32			cfg_base;
	void			*dev;
	struct kvm		*kvm;

	u32			doorbell_offset;
	bool			signal_msi;
	u8			status;
	u8			isr;
	u32			device_features_sel;
	u32			driver_features_sel;

	/*
	 * We cannot rely on the INTERRUPT_LINE byte in the config space once
	 * we have run guest code, as the OS is allowed to use that field
	 * as a scratch pad to communicate between driver and PCI layer.
	 * So store our legacy interrupt line number in here for internal use.
	 */
	u8			legacy_irq_line;

	/* MSI-X */
	u16			config_vector;
	u32			config_gsi;
	u16			vq_vector[VIRTIO_PCI_MAX_VQ];
	u32			gsis[VIRTIO_PCI_MAX_VQ];
	u64			msix_pba;
	/* struct msix_table	msix_table[VIRTIO_PCI_MAX_VQ + VIRTIO_PCI_MAX_CONFIG]; */

	/* virtio queue */
	u16			queue_selector;
	/* struct virtio_pci_ioevent_param ioeventfds[VIRTIO_PCI_MAX_VQ]; */
};

struct kvm_cpu {
};

static inline u32 virtio_pci__mmio_addr(struct virtio_pci *vpci)
{
    return 0;
}

static void inline kvm__irq_line(struct kvm *kvm, int irq, int level)
{
    demu_set_irq(irq, level);
}

static inline int virtio_pci__add_msix_route(struct virtio_pci *vpci, u32 vec)
{
    warn("%s NOT implemented\n", __func__);
    return -1;
}

int virtio_pci_init(struct kvm *kvm, void *dev, struct virtio_device *vdev,
		    int device_id, int subsys_id, int class, u32 addr, u32 irq);

int virtio_pci_exit(struct kvm *kvm, struct virtio_device *vdev);
int virtio_pci_reset(struct kvm *kvm, struct virtio_device *vdev);
int virtio_pci_init_vq(struct kvm *kvm, struct virtio_device *vdev, int vq);
void virtio_pci_exit_vq(struct kvm *kvm, struct virtio_device *vdev, int vq);
int virtio_pci_signal_vq(struct kvm *kvm, struct virtio_device *vdev, u32 vq);
int virtio_pci_signal_config(struct kvm *kvm, struct virtio_device *vdev);

void virtio_pci_modern__io_mmio_callback(struct kvm_cpu *vcpu, u64 addr,
                                         u8 *data, u32 len, u8 is_write,
                                         void *ptr);
#endif  /* _DEVICE_H */


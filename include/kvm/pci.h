#ifndef KVM__PCI_H
#define KVM__PCI_H

#include <linux/types.h>
#include <linux/virtio_pci.h>

#define PCI_IO_SIZE           0x100

struct virtio_caps {
	struct virtio_pci_cap		common;
	struct virtio_pci_notify_cap	notify;
	struct virtio_pci_cap		isr;
	struct virtio_pci_cap		device;
	struct virtio_pci_cfg_cap	pci;
};

#endif /* KVM__PCI_H */

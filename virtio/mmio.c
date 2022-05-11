#include "kvm/virtio-mmio.h"
#include "kvm/virtio.h"
#include "kvm/kvm.h"

#include <linux/virtio_mmio.h>
#include <linux/byteorder.h>
#include <string.h>

#include "../demu.h"

/*
 * XXX:
 * 1. ioeventfd doesn't work without vhost support in kernel.
 * virtio-blk can operate without it, not sure about other virtio backends.
 */

static void kvm__irq_trigger(struct kvm *kvm, int irq)
{
	demu_set_irq(irq, VIRTIO_IRQ_HIGH);
}

#if 0
static void virtio_mmio_ioevent_callback(struct kvm *kvm, void *param)
{
	struct virtio_mmio_ioevent_param *ioeventfd = param;
	struct virtio_mmio *vmmio = ioeventfd->vdev->virtio;

	ioeventfd->vdev->ops->notify_vq(kvm, vmmio->dev, ioeventfd->vq);
}

int virtio_mmio_init_ioeventfd(struct kvm *kvm, struct virtio_device *vdev,
			       u32 vq)
{
	struct virtio_mmio *vmmio = vdev->virtio;
	struct ioevent ioevent;
	int err;

	vmmio->ioeventfds[vq] = (struct virtio_mmio_ioevent_param) {
		.vdev		= vdev,
		.vq		= vq,
	};

	ioevent = (struct ioevent) {
		.io_addr	= vmmio->addr + VIRTIO_MMIO_QUEUE_NOTIFY,
		.io_len		= sizeof(u32),
		.fn		= virtio_mmio_ioevent_callback,
		.fn_ptr		= &vmmio->ioeventfds[vq],
		.datamatch	= vq,
		.fn_kvm		= kvm,
		.fd		= eventfd(0, 0),
	};

	if (vdev->use_vhost)
		/*
		 * Vhost will poll the eventfd in host kernel side,
		 * no need to poll in userspace.
		 */
		err = ioeventfd__add_event(&ioevent, 0);
	else
		/* Need to poll in userspace. */
		err = ioeventfd__add_event(&ioevent, IOEVENTFD_FLAG_USER_POLL);
	if (err)
		return err;

	if (vdev->ops->notify_vq_eventfd)
		vdev->ops->notify_vq_eventfd(kvm, vmmio->dev, vq, ioevent.fd);

	return 0;
}
#endif

int virtio_mmio_signal_vq(struct kvm *kvm, struct virtio_device *vdev, u32 vq)
{
	struct virtio_mmio *vmmio = vdev->virtio;

	vmmio->hdr.interrupt_state |= VIRTIO_MMIO_INT_VRING;
	kvm__irq_trigger(vmmio->kvm, vmmio->irq);

	return 0;
}

int virtio_mmio_init_vq(struct kvm *kvm, struct virtio_device *vdev, int vq)
{
	struct virtio_mmio *vmmio = vdev->virtio;

#if 0
	int ret = virtio_mmio_init_ioeventfd(vmmio->kvm, vdev, vq);
	if (ret) {
		pr_err("couldn't add ioeventfd for vq %d: %d", vq, ret);
		return ret;
	}
#endif
	return vdev->ops->init_vq(vmmio->kvm, vmmio->dev, vq);
}

void virtio_mmio_exit_vq(struct kvm *kvm, struct virtio_device *vdev, int vq)
{
	struct virtio_mmio *vmmio = vdev->virtio;

#if 0
	ioeventfd__del_event(vmmio->addr + VIRTIO_MMIO_QUEUE_NOTIFY, vq);
#endif
	virtio_exit_vq(kvm, vdev, vmmio->dev, vq);
}

int virtio_mmio_signal_config(struct kvm *kvm, struct virtio_device *vdev)
{
	struct virtio_mmio *vmmio = vdev->virtio;

	vmmio->hdr.interrupt_state |= VIRTIO_MMIO_INT_CONFIG;
	kvm__irq_trigger(vmmio->kvm, vmmio->irq);

	return 0;
}

void virtio_mmio_device_specific(u64 addr, u8 *data,
				 u32 len, u8 is_write,
				 struct virtio_device *vdev)
{
	struct virtio_mmio *vmmio = vdev->virtio;

	if (is_write)
		virtio_write_config(vmmio->kvm, vdev, vmmio->dev, addr, data,
				    len);
	else
		virtio_read_config(vmmio->kvm, vdev, vmmio->dev, addr, data,
				   len);
}

int virtio_mmio_init(struct kvm *kvm, void *dev, struct virtio_device *vdev,
		     int device_id, int subsys_id, int class, u32 addr, u32 irq)
{
	bool legacy = vdev->legacy;
	struct virtio_mmio *vmmio = vdev->virtio;
	int r;

	vmmio->addr	= addr;
	vmmio->irq	= irq;
	vmmio->kvm	= kvm;
	vmmio->dev	= dev;

	if (!legacy)
		vdev->endian = VIRTIO_ENDIAN_LE;

	r = demu_register_memory_space(vmmio->addr, VIRTIO_MMIO_IO_SIZE,
			legacy ? virtio_mmio_legacy_callback :
			virtio_mmio_modern_callback, vdev);
	if (r < 0)
		return r;

	vmmio->hdr = (struct virtio_mmio_hdr) {
		.magic		= {'v', 'i', 'r', 't'},
		.version	= legacy ? 1 : 2,
		.device_id	= subsys_id,
		.vendor_id	= 0x4d564b4c , /* 'LKVM' */
		.queue_num_max	= 256,
	};

	/*
	 * Instantiate guest virtio-mmio devices using kernel command line
	 * (or module) parameter, e.g
	 *
	 * virtio_mmio.devices=0x200@0xd2000000:5,0x200@0xd2000200:6
	 */
	pr_debug("virtio-mmio.devices=0x%x@0x%x:%d", VIRTIO_MMIO_IO_SIZE,
		 vmmio->addr, vmmio->irq);

	return 0;
}

int virtio_mmio_reset(struct kvm *kvm, struct virtio_device *vdev)
{
	int vq;
	struct virtio_mmio *vmmio = vdev->virtio;

	for (vq = 0; vq < vdev->ops->get_vq_count(kvm, vmmio->dev); vq++)
		virtio_mmio_exit_vq(kvm, vdev, vq);

	return 0;
}

int virtio_mmio_exit(struct kvm *kvm, struct virtio_device *vdev)
{
	struct virtio_mmio *vmmio = vdev->virtio;

	virtio_mmio_reset(kvm, vdev);
	demu_deregister_memory_space(vmmio->addr);

	return 0;
}

#include "kvm/virtio-blk.h"

#include "kvm/disk-image.h"
#include "kvm/mutex.h"
#include "kvm/util.h"
#include "kvm/kvm.h"
#include "kvm/virtio.h"

#include <sys/eventfd.h>
#include <linux/virtio_ring.h>
#include <linux/virtio_blk.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/types.h>
#include <pthread.h>

#include "../demu.h"

#define PCI_DEVICE_ID_VIRTIO_BLK	0x1001
#define PCI_CLASS_BLK				0x018000

#define VIRTIO_BLK_MAX_DEV		4

extern bool virtio_legacy;

/*
 * the header and status consume too entries
 */
#define DISK_SEG_MAX			(VIRTIO_BLK_QUEUE_SIZE - 2)
#define VIRTIO_BLK_QUEUE_SIZE		256
#define NUM_VIRT_QUEUES			1

struct blk_dev_req {
	struct virt_queue		*vq;
	struct blk_dev			*bdev;
	struct iovec			iov[VIRTIO_BLK_QUEUE_SIZE];
	u16				out, in, head;
	struct kvm			*kvm;
};

struct blk_dev {
	struct mutex			mutex;

	struct list_head		list;

	struct virtio_device		vdev;
	struct virtio_blk_config	blk_config;
	struct disk_image		*disk;

	struct virt_queue		vqs[NUM_VIRT_QUEUES];
	struct blk_dev_req		reqs[VIRTIO_BLK_QUEUE_SIZE];

	pthread_t			io_thread;
	int				io_efd;
	int				io_done;

	struct kvm			*kvm;
};

static LIST_HEAD(bdevs);

void virtio_blk_complete(void *param, long len)
{
	struct blk_dev_req *req = param;
	struct blk_dev *bdev = req->bdev;
	int queueid = req->vq - bdev->vqs;
	u8 *status;

	/* status */
	status	= req->iov[req->out + req->in - 1].iov_base;
	*status	= (len < 0) ? VIRTIO_BLK_S_IOERR : VIRTIO_BLK_S_OK;

#ifndef MAP_IN_ADVANCE
	/* Unmap all descriptors */
	for (int i = 0; i < req->out + req->in; i++)
		demu_unmap_guest_range(req->iov[i].iov_base, req->iov[i].iov_len);
#endif

	mutex_lock(&bdev->mutex);
	virt_queue__set_used_elem(req->vq, req->head, len);
	mutex_unlock(&bdev->mutex);

	if (virtio_queue__should_signal(&bdev->vqs[queueid]))
		bdev->vdev.ops->signal_vq(req->kvm, &bdev->vdev, queueid);
}

static void virtio_blk_do_io_request(struct kvm *kvm, struct virt_queue *vq, struct blk_dev_req *req)
{
	struct virtio_blk_outhdr *req_hdr;
	ssize_t block_cnt;
	struct blk_dev *bdev;
	struct iovec *iov;
	u16 out, in;
	u32 type;
	u64 sector;

	block_cnt	= -1;
	bdev		= req->bdev;
	iov		= req->iov;
	out		= req->out;
	in		= req->in;

	req_hdr		= iov[0].iov_base;

	type = virtio_guest_to_host_u32(vq, req_hdr->type);
	sector = virtio_guest_to_host_u64(vq, req_hdr->sector);

	switch (type) {
	case VIRTIO_BLK_T_IN:
		block_cnt = disk_image__read(bdev->disk, sector,
				iov + 1, in + out - 2, req);
		break;
	case VIRTIO_BLK_T_OUT:
		block_cnt = disk_image__write(bdev->disk, sector,
				iov + 1, in + out - 2, req);
		break;
	case VIRTIO_BLK_T_FLUSH:
		block_cnt = disk_image__flush(bdev->disk);
		virtio_blk_complete(req, block_cnt);
		break;
	case VIRTIO_BLK_T_GET_ID:
		block_cnt = VIRTIO_BLK_ID_BYTES;
		disk_image__get_serial(bdev->disk,
				(iov + 1)->iov_base, &block_cnt);
		virtio_blk_complete(req, block_cnt);
		break;
	default:
		pr_warning("request type %d", type);
		block_cnt	= -1;
		break;
	}
}

static void virtio_blk_do_io(struct kvm *kvm, struct virt_queue *vq, struct blk_dev *bdev)
{
	struct blk_dev_req *req;
	u16 head;

	while (virt_queue__available(vq) && !bdev->io_done) {
		head		= virt_queue__pop(vq);
		req		= &bdev->reqs[head];
		req->head	= virt_queue__get_head_iov(vq, req->iov, &req->out,
					&req->in, head, kvm);
		req->vq		= vq;

		virtio_blk_do_io_request(kvm, vq, req);
	}
}

static u8 *get_config(struct kvm *kvm, void *dev)
{
	struct blk_dev *bdev = dev;

	return ((u8 *)(&bdev->blk_config));
}

static u64 get_host_features(struct kvm *kvm, void *dev)
{
	struct blk_dev *bdev = dev;

	return	1UL << VIRTIO_BLK_F_SEG_MAX
		| 1UL << VIRTIO_BLK_F_FLUSH
		| 1UL << VIRTIO_RING_F_EVENT_IDX
		| 1UL << VIRTIO_RING_F_INDIRECT_DESC
/* XXX */
#if 0
		| 1UL << VIRTIO_F_ANY_LAYOUT
#endif
		| (bdev->disk->readonly ? 1UL << VIRTIO_BLK_F_RO : 0);
}

static void notify_status(struct kvm *kvm, void *dev, u32 status)
{
	struct blk_dev *bdev = dev;
	struct virtio_blk_config *conf = &bdev->blk_config;

	if (!(status & VIRTIO__STATUS_SWAB))
		return;

	conf->capacity = virtio_host_to_guest_u64(&bdev->vdev, conf->capacity);
	conf->size_max = virtio_host_to_guest_u32(&bdev->vdev, conf->size_max);
	conf->seg_max = virtio_host_to_guest_u32(&bdev->vdev, conf->seg_max);

	/* Geometry */
	conf->geometry.cylinders = virtio_host_to_guest_u16(&bdev->vdev,
						conf->geometry.cylinders);

	conf->blk_size = virtio_host_to_guest_u32(&bdev->vdev, conf->blk_size);
	conf->min_io_size = virtio_host_to_guest_u16(&bdev->vdev, conf->min_io_size);
	conf->opt_io_size = virtio_host_to_guest_u32(&bdev->vdev, conf->opt_io_size);
}

static void *virtio_blk_thread(void *dev)
{
	struct blk_dev *bdev = dev;
	u64 data;
	int r;

	kvm__set_thread_name("virtio-blk-io");

	while (!bdev->io_done) {
		r = read(bdev->io_efd, &data, sizeof(u64));
		if (r < 0)
			continue;
		virtio_blk_do_io(bdev->kvm, &bdev->vqs[0], bdev);
	}

	pthread_exit(NULL);
	return NULL;
}

static int init_vq(struct kvm *kvm, void *dev, u32 vq)
{
	unsigned int i;
	struct blk_dev *bdev = dev;

	virtio_init_device_vq(kvm, &bdev->vdev, &bdev->vqs[vq],
			      VIRTIO_BLK_QUEUE_SIZE);

	if (vq != 0)
		return 0;

	for (i = 0; i < ARRAY_SIZE(bdev->reqs); i++) {
		bdev->reqs[i] = (struct blk_dev_req) {
			.bdev = bdev,
			.kvm = kvm,
		};
	}

	mutex_init(&bdev->mutex);
	bdev->io_efd = eventfd(0, 0);
	if (bdev->io_efd < 0)
		return -errno;

	bdev->io_done = 0;
	if (pthread_create(&bdev->io_thread, NULL, virtio_blk_thread, bdev))
		return -errno;

	return 0;
}

static int notify_vq(struct kvm *kvm, void *dev, u32 vq);

static void exit_vq(struct kvm *kvm, void *dev, u32 vq)
{
	struct blk_dev *bdev = dev;

	if (vq != 0)
		return;

	bdev->io_done = 1;
	notify_vq(kvm, dev, vq);
	pthread_join(bdev->io_thread, NULL);
	close(bdev->io_efd);

	disk_image__wait(bdev->disk);
}

static int notify_vq(struct kvm *kvm, void *dev, u32 vq)
{
	struct blk_dev *bdev = dev;
	u64 data = 1;
	int r;

	r = write(bdev->io_efd, &data, sizeof(data));
	if (r < 0)
		return r;

	return 0;
}

static struct virt_queue *get_vq(struct kvm *kvm, void *dev, u32 vq)
{
	struct blk_dev *bdev = dev;

	return &bdev->vqs[vq];
}

static int get_size_vq(struct kvm *kvm, void *dev, u32 vq)
{
	/* FIXME: dynamic */
	return VIRTIO_BLK_QUEUE_SIZE;
}

static int set_size_vq(struct kvm *kvm, void *dev, u32 vq, int size)
{
	/* FIXME: dynamic */
	return size;
}

static int get_vq_count(struct kvm *kvm, void *dev)
{
	return NUM_VIRT_QUEUES;
}

static struct virtio_ops blk_dev_virtio_ops = {
	.get_config		= get_config,
	.get_host_features	= get_host_features,
	.get_vq_count		= get_vq_count,
	.init_vq		= init_vq,
	.exit_vq		= exit_vq,
	.notify_status		= notify_status,
	.notify_vq		= notify_vq,
	.get_vq			= get_vq,
	.get_size_vq		= get_size_vq,
	.set_size_vq		= set_size_vq,
};

static int virtio_blk__init_one(struct kvm *kvm, struct disk_image *disk)
{
	struct blk_dev *bdev;
	int r;

	if (!disk)
		return -EINVAL;

	bdev = calloc(1, sizeof(struct blk_dev));
	if (bdev == NULL)
		return -ENOMEM;

	*bdev = (struct blk_dev) {
		.disk			= disk,
		.blk_config		= (struct virtio_blk_config) {
			.capacity	= disk->size / SECTOR_SIZE,
			.seg_max	= DISK_SEG_MAX,
		},
		.kvm			= kvm,
	};

	list_add_tail(&bdev->list, &bdevs);

	r = virtio_init(kvm, bdev, &bdev->vdev, &blk_dev_virtio_ops,
			virtio_legacy ? VIRTIO_MMIO_LEGACY : VIRTIO_MMIO,
			PCI_DEVICE_ID_VIRTIO_BLK, VIRTIO_ID_BLOCK, PCI_CLASS_BLK,
			disk->addr, disk->irq);
	if (r < 0)
		return r;

	disk_image__set_callback(bdev->disk, virtio_blk_complete);

	return 0;
}

static int virtio_blk__exit_one(struct kvm *kvm, struct blk_dev *bdev)
{
	list_del(&bdev->list);
	bdev->vdev.ops->exit(kvm, &bdev->vdev);
	free(bdev);

	return 0;
}

int virtio_blk__init(struct kvm *kvm)
{
	int i, r = 0;

	for (i = 0; i < kvm->nr_disks; i++) {
		if (kvm->disks[i]->wwpn)
			continue;
		r = virtio_blk__init_one(kvm, kvm->disks[i]);
		if (r < 0)
			goto cleanup;
	}

	return 0;
cleanup:
	virtio_blk__exit(kvm);
	return r;
}
virtio_dev_init(virtio_blk__init);

int virtio_blk__exit(struct kvm *kvm)
{
	while (!list_empty(&bdevs)) {
		struct blk_dev *bdev;

		bdev = list_first_entry(&bdevs, struct blk_dev, list);
		virtio_blk__exit_one(kvm, bdev);
	}

	return 0;
}
virtio_dev_exit(virtio_blk__exit);

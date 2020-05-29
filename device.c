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
#include <fcntl.h>
#include <string.h>
#include <assert.h>
#include <inttypes.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include <xenctrl.h>

#include "debug.h"
#include "demu.h"
#include "device.h"
#include "mapcache.h"

#include "kvm/kvm.h"
#include "kvm/virtio-mmio.h"
#include <linux/err.h>

/*
 * XXX:
 * 1. The initialization of virtio stuff should be refactored.
 */

typedef struct _device_memory_state {
    unsigned int index;
    uint64_t base;
    uint64_t size;
    uint8_t registered;
} device_memory_state_t;

static device_memory_state_t device_memory_state[MAX_DISK_IMAGES];

static uint8_t device_memory_readb(void *priv, uint64_t addr)
{
    device_memory_state_t *state = priv;

    addr -= state->base;

    DBG("%d: offset: 0x%lx\n", state->index, addr);

    return 0;
}

static void device_memory_writeb(void *priv, uint64_t addr, uint8_t val)
{
    device_memory_state_t *state = priv;

    addr -= state->base;

    DBG("%d: offset: 0x%lx val: 0x%x\n", state->index, addr, val);
}

static uint16_t device_memory_readw(void *priv, uint64_t addr)
{
    device_memory_state_t *state = priv;

    addr -= state->base;

    DBG("%d: offset: 0x%lx\n", state->index, addr);

    return 0;
}

static void device_memory_writew(void *priv, uint64_t addr, uint16_t val)
{
    device_memory_state_t *state = priv;

    addr -= state->base;

    DBG("%d: offset: 0x%lx val: 0x%x\n", state->index, addr, val);
}

static uint32_t device_memory_readl(void *priv, uint64_t addr)
{
    device_memory_state_t *state = priv;

    addr -= state->base;

    DBG("%d: offset: 0x%lx\n", state->index, addr);

    return 0;
}

static void device_memory_writel(void *priv, uint64_t addr, uint32_t val)
{
    device_memory_state_t *state = priv;

    addr -= state->base;

    DBG("%d: offset: 0x%lx: val: 0x%x\n", state->index, addr, val);
}

static io_ops_t device_memory_ops = {
    .readb = device_memory_readb,
    .writeb = device_memory_writeb,
    .readw = device_memory_readw,
    .writew = device_memory_writew,
    .readl = device_memory_readl,
    .writel = device_memory_writel
};

static struct kvm *kvm_inst;

static struct kvm *kvm_init(struct disk_image_params *disk_image, u8 image_count)
{
	struct kvm *kvm = calloc(1, sizeof(*kvm));
	int rc;

	if (!kvm)
		return ERR_PTR(-ENOMEM);

	memcpy(kvm->cfg.disk_image, disk_image, sizeof(*disk_image) * image_count);
	kvm->cfg.image_count = image_count;
	kvm->nr_disks = kvm->cfg.image_count;

	rc = init_list__init(kvm);
	if (rc < 0) {
		DBG ("Initialization failed\n");
		free(kvm);
		return ERR_PTR(rc);
	}

	return kvm;
}

static void kvm_exit(struct kvm *kvm)
{
	init_list__exit(kvm);
}

int device_initialize(struct disk_image_params *disk_image, u8 image_count)
{
    int i, rc;

    for (i = 0; i < image_count; i++) {
        if (device_memory_state[i].registered)
            return -1;

        device_memory_state[i].index = i;
        device_memory_state[i].base = disk_image[i].addr;
        device_memory_state[i].size = VIRTIO_MMIO_IO_SIZE;

        rc = demu_register_memory_space(device_memory_state[i].base,
                                        device_memory_state[i].size,
                                        &device_memory_ops,
                                        &device_memory_state[i]);
        if (rc < 0)
            goto fail1;

        device_memory_state[i].registered = 1;
    }

    kvm_inst = kvm_init(disk_image, image_count);
    if (IS_ERR(kvm_inst)) {
        rc = PTR_ERR(kvm_inst);
        goto fail1;
    }

    return 0;

fail1:
    DBG("fail1\n");

    for (i = 0; i < image_count; i++) {
        if (device_memory_state[i].registered) {
            demu_deregister_memory_space(device_memory_state[i].base);
            device_memory_state[i].registered = 0;
        }
    }

    return rc;
}

void device_teardown(void)
{
    int i;

    if (!IS_ERR_OR_NULL(kvm_inst)) {
        kvm_exit(kvm_inst);
        kvm_inst = NULL;
    }

    mapcache_invalidate();

    for (i = 0; i < MAX_DISK_IMAGES; i++) {
        if (device_memory_state[i].registered) {
            demu_deregister_memory_space(device_memory_state[i].base);
            device_memory_state[i].registered = 0;
        }
    }
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * c-tab-always-indent: nil
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */

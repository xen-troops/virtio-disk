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
#include <linux/err.h>

/*
 * XXX:
 * 1. The initialization of virtio stuff should be refactored.
 * 2. All virtio-blk parameters must be configured.
 */

typedef struct _device_memory_state {
    unsigned int index;
    uint64_t base;
    uint64_t size;
    uint8_t registered;
} device_memory_state_t;

static device_memory_state_t device_memory_state;

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

static struct kvm *kvm_init(char *device_str)
{
	struct kvm *kvm = calloc(1, sizeof(*kvm));
	int rc;

	if (!kvm)
		return ERR_PTR(-ENOMEM);

	kvm->cfg.disk_image[0].filename = device_str;
	kvm->cfg.disk_image[0].readonly = 0;
	kvm->cfg.disk_image[0].direct = 0;
	kvm->cfg.disk_image[0].addr = GUEST_VIRTIO_MMIO_BASE;
	kvm->cfg.disk_image[0].irq = GUEST_VIRTIO_MMIO_SPI;

	kvm->cfg.image_count = 1;

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

int device_initialize(char *device_str)
{
    int rc;

    if (device_memory_state.registered)
        return -1;

    device_memory_state.index = 0;
    device_memory_state.base = GUEST_VIRTIO_MMIO_BASE;
    device_memory_state.size = GUEST_VIRTIO_MMIO_SIZE;

    rc = demu_register_memory_space(device_memory_state.base,
                                    device_memory_state.size,
                                    &device_memory_ops,
                                    &device_memory_state);
    if (rc < 0)
        goto fail1;

    device_memory_state.registered = 1;

#ifdef MAP_IN_ADVANCE
    /* either map here or during first demu_get_host_addr request */
    /*demu_map_whole_guest();*/
#endif

    kvm_inst = kvm_init(device_str);
    if (IS_ERR(kvm_inst)) {
        rc = PTR_ERR(kvm_inst);
        goto fail2;
    }

    return 0;

fail2:
    DBG("fail2\n");

    demu_deregister_memory_space(device_memory_state.base);
    device_memory_state.registered = 0;

fail1:
    DBG("fail1\n");

    return rc;
}

void device_teardown(void)
{
    if (!IS_ERR_OR_NULL(kvm_inst)) {
        kvm_exit(kvm_inst);
        kvm_inst = NULL;
    }

#ifdef MAP_IN_ADVANCE
    demu_unmap_whole_guest();
#else
    mapcache_invalidate();
#endif

    if (device_memory_state.registered) {
        demu_deregister_memory_space(device_memory_state.base);
        device_memory_state.registered = 0;
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

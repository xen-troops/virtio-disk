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
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <netinet/in.h>
#include <pwd.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sched.h>
#include <assert.h>
#include <inttypes.h>
#include <pthread.h>

#include <sys/mman.h>
#include <sys/poll.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/prctl.h>

#include <xenctrl.h>
#include <xenforeignmemory.h>
#include <xenevtchn.h>
#include <xendevicemodel.h>
#include <xen/hvm/ioreq.h>
#include <xengnttab.h>

#include "debug.h"
#include "device.h"
#include "demu.h"
#include "xs_dev.h"

#include "kvm/kvm.h"

#define XS_DISK_TYPE	"virtio_disk"

static struct disk_image_params disk_image[MAX_DISK_IMAGES];
static u8 image_count;

bool virtio_legacy = false;

/*
 * XXX:
 * 1. This file should be refactored heavily.
 */

bool do_debug_print = true;

#define __max(_x, _y) (((_x) > (_y)) ? (_x) : (_y))

typedef enum {
    DEMU_SEQ_UNINITIALIZED = 0,
    DEMU_SEQ_XENSTORE_ATTACHED,
    DEMU_SEQ_XENEVTCHN_OPEN,
    DEMU_SEQ_XENFOREIGNMEMORY_OPEN,
    DEMU_SEQ_XENDEVICEMODEL_OPEN,
    DEMU_SEQ_SERVER_REGISTERED,
    DEMU_SEQ_RESOURCE_MAPPED,
    DEMU_SEQ_SERVER_ENABLED,
    DEMU_SEQ_PORT_ARRAY_ALLOCATED,
    DEMU_SEQ_PORTS_BOUND,
#ifdef MAP_IN_ADVANCE
    DEMU_SEQ_GUEST_RAM_MAPPED,
#endif
    DEMU_SEQ_DEVICE_INITIALIZED,
    DEMU_SEQ_INITIALIZED,
    DEMU_NR_SEQS
} demu_seq_t;

typedef struct demu_space demu_space_t;

struct demu_space {
    demu_space_t	*next;
    uint64_t		start;
    uint64_t		end;
    void			(*mmio_fn)(u64 addr, u8 *data, u32 len, u8 is_write, void *ptr);
    void			*ptr;
};

typedef struct demu_state {
    demu_seq_t                       seq;
    xenevtchn_handle                 *xeh;
    xenforeignmemory_handle          *xfh;
    xendevicemodel_handle            *xdh;
    xengnttab_handle                 *xgt;
    int                              use_gnttab;
    domid_t                          domid;
    domid_t                          be_domid;
    unsigned int                     vcpus;
    ioservid_t                       ioservid;
    xenforeignmemory_resource_handle *resource;
    shared_iopage_t                  *shared_iopage;
    evtchn_port_t                    *ioreq_local_port;
    demu_space_t                     *memory;
    struct xs_dev                    *xs_dev;
} demu_state_t;

static demu_state_t demu_state;

void
demu_set_irq(int irq, int level)
{
    xendevicemodel_set_irq_level(demu_state.xdh, demu_state.domid,
                                 irq, level);
}

static void *demu_map_guest_pages(xen_pfn_t pfn[], unsigned int n, int prot)
{
    void *ptr;

    ptr = xenforeignmemory_map(demu_state.xfh, demu_state.domid,
                               prot, n, pfn, NULL);
    if (ptr == NULL)
        DBG("Failed to map pages (pfn 0x%lx count %u prot %d)\n",
            pfn[0], n, prot);

    return ptr;
}

static void demu_unmap_guest_pages(void *ptr, unsigned int n)
{
    int rc;

    rc = xenforeignmemory_unmap(demu_state.xfh, ptr, n);
    if (rc)
        DBG("Failed to unmap pages (va %p count %u)\n", ptr, n);
}

static void *demu_map_guest_grant_refs(uint32_t grants[], unsigned int n, int prot)
{
    void *ptr;

    ptr = xengnttab_map_domain_grant_refs(demu_state.xgt, n, demu_state.domid,
                                          grants, prot);
    if (ptr == NULL)
        DBG("Failed to map grant refs (grant 0x%x count %u prot %d)\n",
            grants[0], n, prot);

    return ptr;
}

static void demu_unmap_guest_grant_refs(void *ptr, unsigned int n)
{
    int rc;

    rc = xengnttab_unmap(demu_state.xgt, ptr, n);
    if (rc)
        DBG("Failed to unmap grant refs (va %p count %u)\n", ptr, n);
}

#define XEN_GRANT_ADDR_OFF   0x8000000000000000ULL

#ifndef MAP_IN_ADVANCE
static void demu_detect_mappings_model(uint64_t addr)
{
    if (demu_state.use_gnttab >= 0)
        return;

    demu_state.use_gnttab = addr & XEN_GRANT_ADDR_OFF ? 1 : 0;

    DBG("Use %s mapping (addr 0x%lx)\n",
        demu_state.use_gnttab > 0 ? "grant" : "foreign", addr);
}
#endif

void *
demu_map_guest_range(uint64_t addr, uint64_t size, int prot)
{
    xen_pfn_t   *pfn = NULL;
    uint32_t    *grants = NULL;
    unsigned int         i, n;
    void        *ptr;

    size = P2ROUNDUP(size, TARGET_PAGE_SIZE);
    n = size >> TARGET_PAGE_SHIFT;

#ifndef MAP_IN_ADVANCE
    demu_detect_mappings_model(addr);
#endif

    if (demu_state.use_gnttab > 0) {
        BUG_ON(!(addr & XEN_GRANT_ADDR_OFF));

        grants = malloc(sizeof (uint32_t) * n);
        if (grants == NULL)
            goto fail1;

        for (i = 0; i < n; i++)
            grants[i] = ((addr & ~XEN_GRANT_ADDR_OFF) >> TARGET_PAGE_SHIFT) + i;

        ptr = demu_map_guest_grant_refs(grants, n, prot);
        if (ptr == NULL)
            goto fail2;

        free(grants);
    } else {
        BUG_ON(addr & XEN_GRANT_ADDR_OFF);

        pfn = malloc(sizeof(xen_pfn_t) * n);
        if (pfn == NULL)
            goto fail1;

        for (i = 0; i < n; i++)
            pfn[i] = (addr >> TARGET_PAGE_SHIFT) + i;

        ptr = demu_map_guest_pages(pfn, n, prot);
        if (ptr == NULL)
            goto fail2;

        free(pfn);
    }

    return ptr + (addr & ~TARGET_PAGE_MASK);

fail2:
    DBG("fail2\n");
    
    if (pfn)
        free(pfn);
    if (grants)
        free(grants);

fail1:
    DBG("fail1\n");

    warn("fail");
    return NULL;
}

int
demu_unmap_guest_range(void *ptr, uint64_t size)
{
    unsigned int n;

    size = P2ROUNDUP(size, TARGET_PAGE_SIZE);
    n = size >> TARGET_PAGE_SHIFT;

    if (demu_state.use_gnttab > 0)
        demu_unmap_guest_grant_refs((void *)((unsigned long)ptr & TARGET_PAGE_MASK), n);
    else
        demu_unmap_guest_pages((void *)((unsigned long)ptr & TARGET_PAGE_MASK), n);

    return 0;
}

#ifdef MAP_IN_ADVANCE
#define NR_GUEST_RAM 2
static void *host_addr[NR_GUEST_RAM];

static uint64_t guest_ram_base[NR_GUEST_RAM];
static uint64_t guest_ram_size[NR_GUEST_RAM];

/* TODO Find a proper way to get guest ram bank info */
static uint64_t xenctrl_get_dom_mem(domid_t domid)
{
	xc_dominfo_t dominfo;
	xc_interface *xch;

	xch = xc_interface_open(NULL, NULL, 0);
	if (xch == NULL)
		return 0;

	if ((xc_domain_getinfo(xch, domid, 1, &dominfo) != 1) ||
			(dominfo.domid != domid)) {
		DBG("Failed to get domain information\n");
		xc_interface_close(xch);
		return 0;
	}

	xc_interface_close(xch);

	return (dominfo.nr_pages - 4) << TARGET_PAGE_SHIFT;
}

static int demu_init_guest_ram(void)
{
	uint64_t mem;

	memset(guest_ram_base, 0, sizeof(guest_ram_base));
	memset(guest_ram_size, 0, sizeof(guest_ram_size));

	mem = xenctrl_get_dom_mem(demu_state.domid);
	if (mem == 0)
		return -1;

	/* #define-s below located at include/public/arch-arm.h */
	guest_ram_base[0] = GUEST_RAM0_BASE;
	if (mem <= GUEST_RAM0_SIZE)
		guest_ram_size[0] = mem;
	else {
		guest_ram_size[0] = GUEST_RAM0_SIZE;
		guest_ram_base[1] = GUEST_RAM1_BASE;
		guest_ram_size[1] = mem - GUEST_RAM0_SIZE;
	}

	return 0;
}

static int demu_map_guest_ram(void)
{
	unsigned int i;

	for (i = 0; i < NR_GUEST_RAM; i++) {
		if (host_addr[i])
			continue;

		if (!guest_ram_base[i] || !guest_ram_size[i])
			continue;

		host_addr[i] = demu_map_guest_range(guest_ram_base[i], guest_ram_size[i],
				PROT_READ | PROT_WRITE);

		if (!host_addr[i]) {
			DBG("Cannot map guest ram%u pa 0x%lx-0x%lx\n",
					i, guest_ram_base[i], guest_ram_base[i] + guest_ram_size[i]);
			goto fail;
		}

		DBG("Mapped guest ram%u pa 0x%lx-0x%lx to va 0x%lx\n",
				i, guest_ram_base[i], guest_ram_base[i] + guest_ram_size[i],
				(unsigned long)host_addr[i]);
	}

	return 0;

fail:
	while (i--) {
		if (host_addr[i]) {
			demu_unmap_guest_range(host_addr[i], guest_ram_size[i]);
			host_addr[i] = NULL;
		}
	}

	return -1;
}

static void demu_unmap_guest_ram(void)
{
	unsigned int i;

	for (i = 0; i < NR_GUEST_RAM; i++) {
		if (!host_addr[i])
			continue;

		demu_unmap_guest_range(host_addr[i], guest_ram_size[i]);
		DBG("Unmapped guest ram%u va 0x%lx\n", i, (unsigned long)host_addr[i]);
		host_addr[i] = NULL;
	}
}

void *demu_get_host_addr(uint64_t offset)
{
	void *addr;
	unsigned int i;

	if (offset & XEN_GRANT_ADDR_OFF) {
		DBG("The highest bit is set in guest pa 0x%lx (MAP_IN_ADVANCE option must be disabled)\n",
				offset);
		BUG_ON(1);
	}

	for (i = 0; i < NR_GUEST_RAM; i++) {
		if (!host_addr[i])
			continue;

		if (offset >= guest_ram_base[i] &&
				offset < guest_ram_base[i] + guest_ram_size[i]) {
			addr = host_addr[i] + (offset - guest_ram_base[i]);
			/*DBG("Translate guest pa 0x%lx to host va 0x%lx\n",
					offset, (unsigned long)addr);*/
			return addr;
		}
	}

	DBG("Cannot translate guest pa 0x%lx\n", offset);

	return NULL;
}
#endif

static demu_space_t *
demu_find_space(demu_space_t *head, uint64_t addr)
{
    demu_space_t    *space;

    for (space = head; space != NULL; space = space->next)
        if (addr >= space->start && addr <= space->end)
            return space;

    return NULL;
}

static demu_space_t *
demu_find_memory_space(uint64_t addr)
{
    demu_space_t    *space;

    space = demu_find_space(demu_state.memory, addr);

    if (space == NULL)
        DBG("failed to find space for 0x%"PRIx64"\n", addr);

    return space;
}

static int
demu_register_space(demu_space_t **headp, uint64_t start, uint64_t end,
    void (*mmio_fn)(u64 addr, u8 *data, u32 len, u8 is_write, void *ptr),
    void *ptr)
{
    demu_space_t    *space;

    assert(mmio_fn);

    if (demu_find_space(*headp, start) || demu_find_space(*headp, end))
        goto fail1;

    space = malloc(sizeof (demu_space_t));
    if (space == NULL)
        goto fail2;

    space->start = start;
    space->end = end;
    space->mmio_fn = mmio_fn;
    space->ptr = ptr;

    space->next = *headp;
    *headp = space;

    return 0;

fail2:
    DBG("fail2\n");

fail1:
    DBG("fail1\n");
    warn("fail");
    return -1;
}

static void
demu_deregister_space(demu_space_t **headp, uint64_t start, uint64_t *end)
{
    demu_space_t    **spacep;
    demu_space_t    *space;

    spacep = headp;
    while ((space = *spacep) != NULL) {
        if (start == space->start) {
            *spacep = space->next;
            if (end != NULL)
                *end = space->end;
            free(space);
            return;
        }
        spacep = &(space->next);
    }
}

int
demu_register_memory_space(uint64_t start, uint64_t size,
    void (*mmio_fn)(u64 addr, u8 *data, u32 len, u8 is_write, void *ptr),
    void *ptr)
{
    int rc;

    DBG("%"PRIx64" - %"PRIx64"\n", start, start + size - 1);

    rc = demu_register_space(&demu_state.memory, start, start + size - 1, mmio_fn, ptr);
    if (rc < 0)
        goto fail1;

    rc = xendevicemodel_map_io_range_to_ioreq_server(demu_state.xdh,
                                                     demu_state.domid,
                                                     demu_state.ioservid,
                                                     1, start,
                                                     start + size - 1);
    if (rc < 0)
        goto fail2;

    return 0;

fail2:
    DBG("fail2\n");

    demu_deregister_space(&demu_state.memory, start, NULL);

fail1:
    DBG("fail1\n");

    warn("fail");
    return -1;
}

void
demu_deregister_memory_space(uint64_t start)
{
    uint64_t end = 0;

    DBG("%"PRIx64"\n", start);

    demu_deregister_space(&demu_state.memory, start, &end);

    xendevicemodel_unmap_io_range_from_ioreq_server(demu_state.xdh,
                                                    demu_state.domid,
                                                    demu_state.ioservid,
                                                    1, start, end);
}

static void
demu_handle_io(ioreq_t *ioreq)
{
    uint8_t data[8] = {0};
    demu_space_t *space;

    space = demu_find_memory_space(ioreq->addr);
    if (space == NULL) {
        fprintf(stderr, "Ignoring MMIO %s at 0x%lx (size %u)\n",
            ioreq->dir == IOREQ_READ ? "read" : "write", ioreq->addr, ioreq->size);
        return;
    }

    if (ioreq->dir == IOREQ_READ) {
        if (!ioreq->data_is_ptr) {
            space->mmio_fn(ioreq->addr, data, ioreq->size, 0, space->ptr);
            ioreq->data = *(uint64_t *)&data;
        } else
            assert(0);
    } else if (ioreq->dir == IOREQ_WRITE) {
        if (!ioreq->data_is_ptr) {
            *(uint64_t *)&data = ioreq->data;
            space->mmio_fn(ioreq->addr, data, ioreq->size, 1, space->ptr);
        } else
            assert(0);
    }
}

static void
demu_handle_ioreq(ioreq_t *ioreq)
{
    switch (ioreq->type) {
    case IOREQ_TYPE_COPY:
        demu_handle_io(ioreq);
        break;

    case IOREQ_TYPE_INVALIDATE:
        /* TODO Remap the whole guest ram once it's memory layout is changed */
        DBG("NOT IMPLEMENTED (%02x)\n", ioreq->type);
        break;

    default:
        DBG("UNKNOWN (%02x)", ioreq->type);
        break;
    }
}

static void
demu_seq_next(void)
{
    assert(demu_state.seq < DEMU_SEQ_INITIALIZED);

    switch (++demu_state.seq) {
    case DEMU_SEQ_XENSTORE_ATTACHED: {
        int i;

        DBG(">XENSTORE_ATTACHED\n");
        DBG("domid = %u\n", demu_state.domid);
        DBG("devid = %u\n", demu_state.xs_dev->devid);

        for (i = 0; i < image_count; i++) {
            DBG("filename[%d] = %s\n", i, disk_image[i].filename);
            DBG("readonly[%d] = %d\n", i, disk_image[i].readonly);
            DBG("base[%d]     = 0x%x\n", i, disk_image[i].addr);
            DBG("irq[%d]      = %u\n", i, disk_image[i].irq);
        }
        break;
    }

    case DEMU_SEQ_XENEVTCHN_OPEN:
        DBG(">XENEVTCHN_OPEN\n");
        break;

    case DEMU_SEQ_XENFOREIGNMEMORY_OPEN:
        DBG(">XENFOREIGNMEMORY_OPEN\n");
        break;

    case DEMU_SEQ_XENDEVICEMODEL_OPEN:
        DBG(">XENDEVICEMODEL_OPEN\n");
        break;

    case DEMU_SEQ_SERVER_REGISTERED:
        DBG(">SERVER_REGISTERED\n");
        DBG("ioservid = %u\n", demu_state.ioservid);
        break;

    case DEMU_SEQ_RESOURCE_MAPPED:
        DBG(">RESOURCE_MAPPED\n");
        DBG("shared_iopage = %p\n", demu_state.shared_iopage);
        break;

    case DEMU_SEQ_SERVER_ENABLED:
        DBG(">SERVER_ENABLED\n");
        break;

    case DEMU_SEQ_PORT_ARRAY_ALLOCATED:
        DBG(">PORT_ARRAY_ALLOCATED\n");
        break;

    case DEMU_SEQ_PORTS_BOUND: {
        int i;

        DBG(">EVTCHN_PORTS_BOUND\n");

        for (i = 0; i < demu_state.vcpus; i++)
            DBG("VCPU%d: %u -> %u\n", i,
                demu_state.shared_iopage->vcpu_ioreq[i].vp_eport,
                demu_state.ioreq_local_port[i]);

        break;
    }

#ifdef MAP_IN_ADVANCE
    case DEMU_SEQ_GUEST_RAM_MAPPED:
        DBG(">GUEST_RAM_MAPPED\n");
        break;
#endif

    case DEMU_SEQ_DEVICE_INITIALIZED:
        DBG(">DEVICE_INITIALIZED\n");
        break;

    case DEMU_SEQ_INITIALIZED:
        DBG(">INITIALIZED\n");
        break;

    default:
        assert(0);
        break;
    }
}

static void
demu_teardown(void)
{
    if (demu_state.seq == DEMU_SEQ_INITIALIZED) {
        DBG("<INITIALIZED\n");

        demu_state.seq = DEMU_SEQ_DEVICE_INITIALIZED;
    }

    if (demu_state.seq == DEMU_SEQ_DEVICE_INITIALIZED) {
        DBG("<DEVICE_INITIALIZED\n");
        device_teardown();

#ifdef MAP_IN_ADVANCE
        demu_state.seq = DEMU_SEQ_GUEST_RAM_MAPPED;
    }

    if (demu_state.seq == DEMU_SEQ_GUEST_RAM_MAPPED) {
        DBG("<GUEST_RAM_MAPPED\n");

        demu_unmap_guest_ram();

        demu_state.seq = DEMU_SEQ_PORTS_BOUND;
    }
#else
        demu_state.seq = DEMU_SEQ_PORTS_BOUND;
    }
#endif

    if (demu_state.seq >= DEMU_SEQ_PORTS_BOUND) {
        DBG("<EVTCHN_PORTS_BOUND\n");

        demu_state.seq = DEMU_SEQ_PORT_ARRAY_ALLOCATED;
    }

    if (demu_state.seq >= DEMU_SEQ_PORT_ARRAY_ALLOCATED) {
        int i;

        DBG("<PORT_ARRAY_ALLOCATED\n");

        for (i = 0; i < demu_state.vcpus; i++) {
            evtchn_port_t   port;

            port = demu_state.ioreq_local_port[i];

            if (port >= 0) {
                DBG("VCPU%d: %u\n", i, port);
                (void) xenevtchn_unbind(demu_state.xeh, port);
            }
        }

        free(demu_state.ioreq_local_port);

        demu_state.seq = DEMU_SEQ_SERVER_ENABLED;
    }

    if (demu_state.seq == DEMU_SEQ_SERVER_ENABLED) {
        DBG("<SERVER_ENABLED\n");
        (void) xendevicemodel_set_ioreq_server_state(demu_state.xdh,
                                                     demu_state.domid,
                                                     demu_state.ioservid,
                                                     0);

        demu_state.seq = DEMU_SEQ_RESOURCE_MAPPED;
    }

    if (demu_state.seq >= DEMU_SEQ_RESOURCE_MAPPED) {
        DBG("<RESOURCE_MAPPED\n");

        xenforeignmemory_unmap_resource(demu_state.xfh,
                                        demu_state.resource);

        demu_state.seq = DEMU_SEQ_SERVER_REGISTERED;
    }

    if (demu_state.seq >= DEMU_SEQ_SERVER_REGISTERED) {
        DBG("<SERVER_REGISTERED\n");

        (void) xendevicemodel_destroy_ioreq_server(demu_state.xdh,
                                                   demu_state.domid,
                                                   demu_state.ioservid);
        demu_state.seq = DEMU_SEQ_XENDEVICEMODEL_OPEN;
    }

    if (demu_state.seq >= DEMU_SEQ_XENDEVICEMODEL_OPEN) {
        DBG("<XENDEVICEMODEL_OPEN\n");

        xengnttab_close(demu_state.xgt);

        xendevicemodel_close(demu_state.xdh);

        demu_state.seq = DEMU_SEQ_XENFOREIGNMEMORY_OPEN;
    }

    if (demu_state.seq >= DEMU_SEQ_XENFOREIGNMEMORY_OPEN) {
        DBG("<XENFOREIGNMEMORY_OPEN\n");

        xenforeignmemory_close(demu_state.xfh);

        demu_state.seq = DEMU_SEQ_XENEVTCHN_OPEN;
    }

    if (demu_state.seq >= DEMU_SEQ_XENEVTCHN_OPEN) {
        DBG("<XENEVTCHN_OPEN\n");

        xenevtchn_close(demu_state.xeh);

        demu_state.seq = DEMU_SEQ_XENSTORE_ATTACHED;
    }

    if (demu_state.seq >= DEMU_SEQ_XENSTORE_ATTACHED) {
        int i;

        DBG("<XENSTORE_ATTACHED\n");

        for (i = 0; i < MAX_DISK_IMAGES; i++) {
            if (disk_image[i].filename) {
                free((void *)disk_image[i].filename);
                disk_image[i].filename = NULL;
            }
        }

        xenstore_disconnect_dom(demu_state.xs_dev);

        demu_state.seq = DEMU_SEQ_UNINITIALIZED;
    }
}

static struct sigaction sigterm_handler;

static void
demu_sigterm(int num)
{
    DBG("%s\n", strsignal(num));

    demu_teardown();
    xenstore_destroy(demu_state.xs_dev);

    exit(0);
}

static int demu_read_xenstore_config(void *unused)
{
    char *str;
    int val, ret = 0;

    image_count = 0;

    str = xenstore_read_be_str(demu_state.xs_dev, "mode");
    if (!str)
        return -1;
    disk_image[image_count].readonly = !strchr(str, 'w');
    free(str);

    ret = xenstore_read_be_int(demu_state.xs_dev, "base", &val);
    if (ret < 0)
        return ret;
    disk_image[image_count].addr = val;

    ret = xenstore_read_be_int(demu_state.xs_dev, "irq", &val);
    if (ret < 0)
        return ret;
    disk_image[image_count].irq = val;

    str = xenstore_read_be_str(demu_state.xs_dev, "params");
    if (!str)
        return -1;
    disk_image[image_count].filename = str;

    image_count ++;

    return ret;
}

static int
demu_initialize(void)
{
    int             rc;
    void            *addr;
    evtchn_port_t   port;
    int             i;

    rc = xenstore_connect_dom(demu_state.xs_dev, demu_state.be_domid,
            demu_state.domid, demu_read_xenstore_config, NULL);
    if (rc < 0)
        goto fail0;

    demu_seq_next();

    demu_state.xeh = xenevtchn_open(NULL, 0);
    if (demu_state.xeh == NULL)
        goto fail1;

    demu_seq_next();

    demu_state.xfh = xenforeignmemory_open(NULL, 0);
    if (demu_state.xfh == NULL)
        goto fail2;

    demu_seq_next();

    demu_state.xdh = xendevicemodel_open(NULL, 0);
    if (demu_state.xdh == NULL)
        goto fail3;

    demu_state.use_gnttab = -1;
    demu_state.xgt = xengnttab_open(NULL, 0);
    if (demu_state.xgt == NULL)
        goto fail4;

    demu_seq_next();

    rc = xendevicemodel_nr_vcpus(demu_state.xdh, demu_state.domid, &demu_state.vcpus);
    if (rc < 0)
        goto fail4;

    DBG("%d vCPU(s)\n", demu_state.vcpus);

    rc = xendevicemodel_create_ioreq_server(demu_state.xdh,
                                            demu_state.domid, HVM_IOREQSRV_BUFIOREQ_OFF,
                                            &demu_state.ioservid);
    if (rc < 0)
        goto fail5;
    
    demu_seq_next();

    addr = NULL;
    demu_state.resource =
        xenforeignmemory_map_resource(demu_state.xfh, demu_state.domid,
                                      XENMEM_resource_ioreq_server,
                                      demu_state.ioservid,
                                      XENMEM_resource_ioreq_server_frame_ioreq(0), 1,
                                      &addr,
                                      PROT_READ | PROT_WRITE, 0);
    if (demu_state.resource == NULL)
        goto fail6;

    demu_state.shared_iopage = addr;

    demu_seq_next();

    rc = xendevicemodel_set_ioreq_server_state(demu_state.xdh,
                                               demu_state.domid,
                                               demu_state.ioservid,
                                               1);
    if (rc != 0)
        goto fail7;

    demu_seq_next();

    demu_state.ioreq_local_port = malloc(sizeof (evtchn_port_t) *
                                         demu_state.vcpus);
    if (demu_state.ioreq_local_port == NULL)
        goto fail8;

    for (i = 0; i < demu_state.vcpus; i++)
        demu_state.ioreq_local_port[i] = -1;

    demu_seq_next();

    for (i = 0; i < demu_state.vcpus; i++) {
        port = demu_state.shared_iopage->vcpu_ioreq[i].vp_eport;

        rc = xenevtchn_bind_interdomain(demu_state.xeh, demu_state.domid,
                                        port);
        if (rc < 0)
            goto fail9;

        demu_state.ioreq_local_port[i] = rc;
    }

    demu_seq_next();

#ifdef MAP_IN_ADVANCE
    rc = demu_init_guest_ram();
    if (rc < 0)
        goto fail10;

    rc = demu_map_guest_ram();
    if (rc < 0)
        goto fail10;

    demu_seq_next();
#endif

    rc = device_initialize(disk_image, image_count);
    if (rc < 0)
        goto fail11;

    demu_seq_next();

    demu_seq_next();

    assert(demu_state.seq == DEMU_SEQ_INITIALIZED);
    return 0;

fail11:
    DBG("fail11\n");

#ifdef MAP_IN_ADVANCE
fail10:
    DBG("fail10\n");
#endif

fail9:
    DBG("fail9\n");

fail8:
    DBG("fail8\n");

fail7:
    DBG("fail7\n");

fail6:
    DBG("fail6\n");

fail5:
    DBG("fail5\n");

fail4:
    DBG("fail4\n");

fail3:
    DBG("fail3\n");

fail2:
    DBG("fail2\n");

fail1:
    DBG("fail1\n");

fail0:
    DBG("fail0\n");

    warn("fail");
    return -1;
}

static void
demu_poll_shared_iopage(unsigned int i)
{
    ioreq_t *ioreq;

    if (demu_state.seq != DEMU_SEQ_INITIALIZED)
        return;

    ioreq = &demu_state.shared_iopage->vcpu_ioreq[i];
    if (ioreq->state != STATE_IOREQ_READY) {
        fprintf(stderr, "IO request not ready\n");
        return;
    }

    xen_mb();

    ioreq->state = STATE_IOREQ_INPROCESS;

    demu_handle_ioreq(ioreq);
    xen_mb();

    ioreq->state = STATE_IORESP_READY;
    xen_mb();

    xenevtchn_notify(demu_state.xeh, demu_state.ioreq_local_port[i]);
}

static void
demu_poll_iopages(void)
{
    evtchn_port_t   port;
    int             i;

    if (demu_state.seq != DEMU_SEQ_INITIALIZED)
        return;

    port = xenevtchn_pending(demu_state.xeh);
    if (port < 0)
        return;

    for (i = 0; i < demu_state.vcpus; i++) {
        if (port == demu_state.ioreq_local_port[i]) {
            xenevtchn_unmask(demu_state.xeh, port);
            demu_poll_shared_iopage(i);
        }
    }
}

int
main(int argc, char **argv, char **envp)
{
    sigset_t        block;
    int             rc;
    int             efd, xfd;
    char            *devid_str = NULL;
    int             opt;
    const struct option lopts[] =
    {
        {"help", no_argument, NULL, 'h'},
        {"devid", optional_argument, NULL, 'd'},
        {"legacy", no_argument, NULL, 'l'},
        {NULL, 0, NULL, 0},
    };

    while ((opt = getopt_long(argc, argv, "hd:l", lopts, NULL)) != -1) {
        switch (opt) {
            case 'd':
                devid_str = optarg;
                break;

            case 'l':
                virtio_legacy = true;
                break;

            case 'h':
                /* Fallthough */
            default:
                printf("Usage: %s [-d <devid>] [-l (virtio_legacy)]\n", argv[0]);
                return 0;
        }
    }

    sigfillset(&block);

    memset(&sigterm_handler, 0, sizeof (struct sigaction));
    sigterm_handler.sa_handler = demu_sigterm;

    sigaction(SIGTERM, &sigterm_handler, NULL);
    sigdelset(&block, SIGTERM);

    sigaction(SIGINT, &sigterm_handler, NULL);
    sigdelset(&block, SIGINT);

    sigaction(SIGHUP, &sigterm_handler, NULL);
    sigdelset(&block, SIGHUP);

    sigaction(SIGABRT, &sigterm_handler, NULL);
    sigdelset(&block, SIGABRT);

    sigprocmask(SIG_BLOCK, &block, NULL);

    demu_state.xs_dev = xenstore_create(XS_DISK_TYPE, devid_str);
    if (demu_state.xs_dev == NULL) {
        fprintf(stderr, "failed to create xenstore instance\n");
        exit(1);
    }

    rc = xenstore_get_be_domid(demu_state.xs_dev);
    if (rc < 0) {
        xenstore_destroy(demu_state.xs_dev);
        fprintf(stderr, "failed to read backend domid\n");
        exit(1);
    }
    demu_state.be_domid = rc;
    DBG("read backend domid %u\n", demu_state.be_domid);

    while (1) {
        rc = xenstore_wait_fe_domid(demu_state.xs_dev);
        if (rc < 0) {
            /*fprintf(stderr, "failed to read frontend domid\n");*/
            msleep(100);
            continue;
        }
        demu_state.domid = rc;
        DBG("read frontend domid %u\n", demu_state.domid);

        rc = demu_initialize();
        if (rc < 0) {
            demu_teardown();
            continue;
        }

        efd = xenevtchn_fd(demu_state.xeh);
        xfd = xenstore_get_fd(demu_state.xs_dev);

        while (1) {
            int nfds;
            fd_set fds;
            struct timeval t = { .tv_sec = 1 };

            FD_ZERO(&fds);
            FD_SET(efd, &fds);
            FD_SET(xfd, &fds);
            nfds = __max(efd, xfd) + 1;

            rc = select(nfds, &fds, NULL, NULL, &t);
            if (rc > 0) {
                if (FD_ISSET(efd, &fds))
                    demu_poll_iopages();

                if (FD_ISSET(xfd, &fds)) {
                    rc = xenstore_poll_watches(demu_state.xs_dev);
                    if (rc < 0) {
                        DBG("lost connection to dom%d\n", demu_state.domid);
                        rc = 0;
                        break;
                    }
                }
            }

            if (rc < 0 && errno != EINTR)
                break;
        }

        demu_teardown();

        if (rc < 0)
           break;
    }

    xenstore_destroy(demu_state.xs_dev);

    return 0;
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

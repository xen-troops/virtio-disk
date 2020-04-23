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

#include "debug.h"
#include "device.h"
#include "demu.h"

#include "kvm/kvm.h"

/*
 * XXX:
 * 1. This file should be refactored heavily.
 * 2. There must be ability to reconnect to the newly created domain.
 * 3. There must be ability to release domain resources if one goes away.
 * 4. Backend should be able to maintain several instances.
 * 5. Sync with recent kvmtool changes.
 */

static unsigned long count;

bool do_debug_print = true;

#ifdef MAP_IN_ADVANCE
static void *host_addr;

/* We assume guest's address space as following */
#define GUEST_BASE 0x40000000
#define GUEST_SIZE 0x20000000

void demu_map_whole_guest(void)
{
	if (host_addr)
		return;

	host_addr = demu_map_guest_range(GUEST_BASE, GUEST_SIZE);
	if (!host_addr) {
		DBG("Cannot map guest memory\n");
		assert(FALSE);
	}
}

void demu_unmap_whole_guest(void)
{
	if (host_addr) {
		demu_unmap_guest_range(host_addr, GUEST_SIZE);
		host_addr = NULL;
	}
}

void *demu_get_host_addr(uint64_t offset)
{
	void *addr;

	demu_map_whole_guest();

	if (offset >= GUEST_BASE && offset < GUEST_BASE + GUEST_SIZE) {
		addr = host_addr + (offset - GUEST_BASE);
		/*DBG("translate guest 0x%llx to host 0x%llx\n",
				(unsigned long long)offset, (unsigned long long)addr);*/
		return addr;
	}

	DBG("Cannot translate guest 0x%lx", (unsigned long)offset);
	assert(FALSE);

	return NULL;
}
#endif

#define mb() asm volatile ("" : : : "memory")

enum {
    DEMU_OPT_DOMAIN,
    DEMU_OPT_DEVICE,
    DEMU_NR_OPTS
    };

static struct option demu_option[] = {
    {"domain", 1, NULL, 0},
    {"device", 1, NULL, 0},
    {NULL, 0, NULL, 0}
};

static const char *demu_option_text[] = {
    "<domid>",
    "<device>",
    NULL
};

static const char *prog;

static void
usage(void)
{
    int i;

    fprintf(stderr, "Usage: %s <options>\n\n", prog);

    for (i = 0; i < DEMU_NR_OPTS; i++)
        fprintf(stderr, "\t--%s %s\n",
                demu_option[i].name,
                demu_option_text[i]);

    fprintf(stderr, "\n");

    exit(2);
}

typedef enum {
    DEMU_SEQ_UNINITIALIZED = 0,
    DEMU_SEQ_XENCTRL_OPEN,
    DEMU_SEQ_XENEVTCHN_OPEN,
    DEMU_SEQ_XENFOREIGNMEMORY_OPEN,
    DEMU_SEQ_XENDEVICEMODEL_OPEN,
    DEMU_SEQ_SERVER_REGISTERED,
    DEMU_SEQ_RESOURCE_MAPPED,
    DEMU_SEQ_SERVER_ENABLED,
    DEMU_SEQ_PORT_ARRAY_ALLOCATED,
    DEMU_SEQ_PORTS_BOUND,
    DEMU_SEQ_BUF_PORT_BOUND,
    DEMU_SEQ_DEVICE_INITIALIZED,
    DEMU_SEQ_INITIALIZED,
    DEMU_NR_SEQS
} demu_seq_t;

typedef struct demu_space demu_space_t;

struct demu_space {
    demu_space_t	*next;
    uint64_t		start;
    uint64_t		end;
    const io_ops_t	*ops;
    void		    *priv;
};

typedef struct demu_state {
    demu_seq_t                       seq;
    xc_interface                     *xch;
    xenevtchn_handle                 *xeh;
    xenforeignmemory_handle          *xfh;
    xendevicemodel_handle            *xdh;
    domid_t                          domid;
    unsigned int                     vcpus;
    ioservid_t                       ioservid;
    xenforeignmemory_resource_handle *resource;
    shared_iopage_t                  *shared_iopage;
    evtchn_port_t                    *ioreq_local_port;
    buffered_iopage_t                *buffered_iopage;
    evtchn_port_t                    buf_ioreq_port;
    evtchn_port_t                    buf_ioreq_local_port;
    demu_space_t                     *memory;
} demu_state_t;

static demu_state_t demu_state;

void
demu_set_irq(int irq, int level)
{
    xendevicemodel_set_irq_level(demu_state.xdh, demu_state.domid,
                                 irq, level);
}

static inline void *
demu_map_guest_pages(xen_pfn_t pfn[], int err[], unsigned int n)
{
    return xenforeignmemory_map(demu_state.xfh, demu_state.domid,
                                PROT_READ | PROT_WRITE, n,
                                pfn, err);
}

void *
demu_map_guest_range(uint64_t addr, uint64_t size)
{
    xen_pfn_t   *pfn;
    int         i, n, *err;
    void        *ptr;

    count ++;

    /*DBG("%"PRIx64"+%"PRIx64" (%lu)\n", addr, size, count);*/

    size = P2ROUNDUP(size, TARGET_PAGE_SIZE);
    n = size >> TARGET_PAGE_SHIFT;

    pfn = malloc(sizeof (xen_pfn_t) * n);
    if (pfn == NULL)
        goto fail1;

    err = malloc(sizeof (int) * n);
    if (err == NULL)
        goto fail2;

    for (i = 0; i < n; i++)
        pfn[i] = (addr >> TARGET_PAGE_SHIFT) + i;
    
    ptr = demu_map_guest_pages(pfn, err, n);
    if (ptr == NULL) {
        for (i = 0; i < n; i++) {
            if (err[i])
                DBG("Failed to map pfn[%d] %"PRIx64": %d\n", i, pfn[i], err[i]);
        }
        goto fail3;
    }
    
    free(err);
    free(pfn);

    return ptr + (addr % TARGET_PAGE_SIZE);
    
fail3:
    DBG("fail3\n");

    free(err);

fail2:
    DBG("fail2\n");
    
    free(pfn);
    
fail1:
    DBG("fail1\n");

    warn("fail");
    return NULL;
}

static inline void
demu_unmap_guest_pages(void *ptr, unsigned int n)
{
    xenforeignmemory_unmap(demu_state.xfh, ptr, n);
}

int
demu_unmap_guest_range(void *ptr, uint64_t size)
{
    int n;

    count --;

    /*DBG("%p+%"PRIx64" (%lu)\n", ptr, size, count);*/

    size = P2ROUNDUP(size, TARGET_PAGE_SIZE);
    n = size >> TARGET_PAGE_SHIFT;

    demu_unmap_guest_pages((void *)((unsigned long)ptr & TARGET_PAGE_MASK), n);

    return 0;
}

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
                    const io_ops_t *ops, void *priv)
{
    demu_space_t    *space;

    if (demu_find_space(*headp, start) || demu_find_space(*headp, end))
        goto fail1;

    space = malloc(sizeof (demu_space_t));
    if (space == NULL)
        goto fail2;

    space->start = start;
    space->end = end;
    space->ops = ops;
    space->priv = priv;

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
                           const io_ops_t *ops, void *priv)
{
    int rc;

    DBG("%"PRIx64" - %"PRIx64"\n", start, start + size - 1);

    rc = demu_register_space(&demu_state.memory, start, start + size - 1, ops, priv);
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

#define DEMU_IO_READ(_fn, _priv, _addr, _size, _count, _val)        \
    do {                                                            \
        int       		_i = 0;                                     \
        unsigned int	_shift = 0;                                 \
                                                                    \
        (_val) = 0;                                                 \
        for (_i = 0; _i < (_count); _i++)                           \
        {                                                           \
            (_val) |= (uint64_t)(_fn)((_priv), (_addr)) << _shift;  \
            _shift += 8 * (_size);                                  \
            (_addr) += (_size);                                     \
        }                                                           \
    } while (FALSE)

uint64_t
demu_io_read(demu_space_t *space, uint64_t addr, uint64_t size)
{
    uint64_t    val = ~0ull;

    switch (size) {
    case 1:
        val = space->ops->readb(space->priv, addr);
        break;

    case 2:
        if (space->ops->readw == NULL)
            DEMU_IO_READ(space->ops->readb, space->priv, addr, 1, 2, val);
        else
            DEMU_IO_READ(space->ops->readw, space->priv, addr, 2, 1, val);
        break;

    case 4:
        if (space->ops->readl == NULL) {
            if (space->ops->readw == NULL)
                DEMU_IO_READ(space->ops->readb, space->priv, addr, 1, 4,
                             val);
            else
                DEMU_IO_READ(space->ops->readw, space->priv, addr, 2, 2,
                             val);
        } else {
            DEMU_IO_READ(space->ops->readl, space->priv, addr, 4, 1, val);
        }
        break;

    case 8:
        if (space->ops->readl == NULL) {
            if (space->ops->readw == NULL)
                DEMU_IO_READ(space->ops->readb, space->priv, addr, 1, 8,
                             val);
            else
                DEMU_IO_READ(space->ops->readw, space->priv, addr, 2, 4,
                             val);
        } else {
            DEMU_IO_READ(space->ops->readl, space->priv, addr, 4, 2, val);
        }
        break;

    default:
        break;
    }

    return val;
}

#define DEMU_IO_WRITE(_fn, _priv, _addr, _size, _count, _val)   \
    do {                                                        \
        int             _i = 0;                                 \
        unsigned int    _shift = 0;                             \
                                                                \
        for (_i = 0; _i < (_count); _i++)                       \
        {                                                       \
            (_fn)((_priv), (_addr), (_val) >> _shift);          \
            _shift += 8 * (_size);                              \
            (_addr) += (_size);                                 \
        }                                                       \
    } while (FALSE)

void
demu_io_write(demu_space_t *space, uint64_t addr, uint64_t size,
              uint64_t val)
{
    switch (size) {
    case 1:
        space->ops->writeb(space->priv, addr, val);
        break;

    case 2:
        if (space->ops->writew == NULL)
            DEMU_IO_WRITE(space->ops->writeb, space->priv, addr, 1, 2,
                          val);
        else
            DEMU_IO_WRITE(space->ops->writew, space->priv, addr, 2, 1,
                          val);
        break;

    case 4:
        if (space->ops->writel == NULL) {
            if (space->ops->writew == NULL)
                DEMU_IO_WRITE(space->ops->writeb, space->priv, addr, 1, 4,
                              val);
            else
                DEMU_IO_WRITE(space->ops->writew, space->priv, addr, 2, 2,
                              val);
        } else {
            DEMU_IO_WRITE(space->ops->writel, space->priv, addr, 4, 1,
                          val);
        }
        break;

    case 8:
        if (space->ops->writel == NULL) {
            if (space->ops->writew == NULL)
                DEMU_IO_WRITE(space->ops->writeb, space->priv, addr, 1, 8,
                              val);
            else
                DEMU_IO_WRITE(space->ops->writew, space->priv, addr, 2, 4,
                              val);
        } else {
            DEMU_IO_WRITE(space->ops->writel, space->priv, addr, 4, 2,
                          val);
        }
        break;

    default:
        break;
    }
}

static void
demu_handle_io(demu_space_t *space, ioreq_t *ioreq, int is_mmio)
{
    uint8_t data[8] = {0};

    if (space == NULL)
        goto fail1;

    if (ioreq->dir == IOREQ_READ) {
        if (!ioreq->data_is_ptr) {
            /*ioreq->data = demu_io_read(space, ioreq->addr, ioreq->size);*/

            kvm__emulate_mmio(NULL, ioreq->addr, data, ioreq->size, 0);
            ioreq->data = *(uint64_t *)&data;
        } else {
            assert(FALSE);
        }
    } else if (ioreq->dir == IOREQ_WRITE) {
        if (!ioreq->data_is_ptr) {
            /*demu_io_write(space, ioreq->addr, ioreq->size, ioreq->data);*/

            *(uint64_t *)&data = ioreq->data;
            kvm__emulate_mmio(NULL, ioreq->addr, data, ioreq->size, 1);
        } else {
            assert(FALSE);
        }
    }

    return;

fail1:
    DBG("fail1\n");
}

static void
demu_handle_ioreq(ioreq_t *ioreq)
{
    demu_space_t    *space;

    switch (ioreq->type) {
    case IOREQ_TYPE_COPY:
        space = demu_find_memory_space(ioreq->addr);
        demu_handle_io(space, ioreq, TRUE);
        break;

    /* XXX: Do we need this? */
    case IOREQ_TYPE_INVALIDATE:
        assert(FALSE);
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
    case DEMU_SEQ_XENCTRL_OPEN:
        DBG(">XENCTRL_OPEN\n");
        break;

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
        DBG("buffered_iopage = %p\n", demu_state.buffered_iopage);
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

    case DEMU_SEQ_BUF_PORT_BOUND:
        DBG(">EVTCHN_BUF_PORT_BOUND\n");

        DBG("%u -> %u\n",
            demu_state.buf_ioreq_port,
            demu_state.buf_ioreq_local_port);
        break;

    case DEMU_SEQ_DEVICE_INITIALIZED:
        DBG(">DEVICE_INITIALIZED\n");
        break;

    case DEMU_SEQ_INITIALIZED:
        DBG(">INITIALIZED\n");
        break;

    default:
        assert(FALSE);
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

        demu_state.seq = DEMU_SEQ_BUF_PORT_BOUND;
    }

    if (demu_state.seq >= DEMU_SEQ_BUF_PORT_BOUND) {
        DBG("<EVTCHN_BUF_PORT_BOUND\n");
        evtchn_port_t   port;

        port = demu_state.buf_ioreq_local_port;

        DBG("%u\n", port);
        (void) xenevtchn_unbind(demu_state.xeh, port);

        demu_state.seq = DEMU_SEQ_PORTS_BOUND;
    }

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

        demu_state.seq = DEMU_SEQ_XENCTRL_OPEN;
    }

    if (demu_state.seq >= DEMU_SEQ_XENCTRL_OPEN) {
        DBG("<XENCTRL_OPEN\n");

        xc_interface_close(demu_state.xch);

        demu_state.seq = DEMU_SEQ_UNINITIALIZED;
    }
}

static struct sigaction sigterm_handler;

static void
demu_sigterm(int num)
{
    DBG("%s\n", strsignal(num));

    demu_teardown();

    exit(0);
}

static int
demu_initialize(domid_t domid, char *device_str)
{
    int             rc;
    xc_dominfo_t    dominfo;
    void            *addr;
    evtchn_port_t   port;
    evtchn_port_t   buf_port;
    int             i;

    demu_state.domid = domid;

    demu_state.xch = xc_interface_open(NULL, NULL, 0);
    if (demu_state.xch == NULL)
        goto fail1;

    demu_seq_next();

    demu_state.xeh = xenevtchn_open(NULL, 0);
    if (demu_state.xeh == NULL)
        goto fail2;

    demu_seq_next();

    demu_state.xfh = xenforeignmemory_open(NULL, 0);
    if (demu_state.xfh == NULL)
        goto fail3;

    demu_seq_next();

    demu_state.xdh = xendevicemodel_open(NULL, 0);
    if (demu_state.xdh == NULL)
        goto fail4;

    demu_seq_next();

    rc = xc_domain_getinfo(demu_state.xch, demu_state.domid, 1, &dominfo);
    if (rc < 0 || dominfo.domid != demu_state.domid)
        goto fail5;

    demu_state.vcpus = dominfo.max_vcpu_id + 1;

    DBG("%d vCPU(s)\n", demu_state.vcpus);

    /* XXX: Recognize self domain_id */
    rc = xc_domain_set_target(demu_state.xch, 1, demu_state.domid);
    if (rc < 0)
        goto fail5;

    rc = xendevicemodel_create_ioreq_server(demu_state.xdh,
                                            demu_state.domid, 1,
                                            &demu_state.ioservid);
    if (rc < 0)
        goto fail6;
    
    demu_seq_next();

    addr = NULL;
    demu_state.resource =
        xenforeignmemory_map_resource(demu_state.xfh, demu_state.domid,
                                      XENMEM_resource_ioreq_server,
                                      demu_state.ioservid, 0, 2,
                                      &addr,
                                      PROT_READ | PROT_WRITE, 0);
    if (demu_state.resource == NULL)
        goto fail7;

    demu_state.buffered_iopage = addr;
    demu_state.shared_iopage = addr + XC_PAGE_SIZE;

    rc = xendevicemodel_get_ioreq_server_info(demu_state.xdh,
                                              demu_state.domid,
                                              demu_state.ioservid,
                                              NULL, NULL, &buf_port);
    if (rc < 0)
        goto fail8;

    demu_seq_next();

    rc = xendevicemodel_set_ioreq_server_state(demu_state.xdh,
                                               demu_state.domid,
                                               demu_state.ioservid,
                                               1);
    if (rc != 0)
        goto fail9;

    demu_seq_next();

    demu_state.ioreq_local_port = malloc(sizeof (evtchn_port_t) *
                                         demu_state.vcpus);
    if (demu_state.ioreq_local_port == NULL)
        goto fail10;

    for (i = 0; i < demu_state.vcpus; i++)
        demu_state.ioreq_local_port[i] = -1;

    demu_seq_next();

    for (i = 0; i < demu_state.vcpus; i++) {
        port = demu_state.shared_iopage->vcpu_ioreq[i].vp_eport;

        rc = xenevtchn_bind_interdomain(demu_state.xeh, demu_state.domid,
                                        port);
        if (rc < 0)
            goto fail11;

        demu_state.ioreq_local_port[i] = rc;
    }

    demu_seq_next();

    rc = xenevtchn_bind_interdomain(demu_state.xeh, demu_state.domid,
                                    buf_port);
    if (rc < 0)
        goto fail12;

    demu_state.buf_ioreq_local_port = rc;

    demu_seq_next();

    rc = device_initialize(device_str);
    if (rc < 0)
        goto fail13;

    demu_seq_next();

    demu_seq_next();

    assert(demu_state.seq == DEMU_SEQ_INITIALIZED);
    return 0;

fail13:
    DBG("fail13\n");

fail12:
    DBG("fail12\n");

fail11:
    DBG("fail11\n");

fail10:
    DBG("fail10\n");

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

    warn("fail");
    return -1;
}

static void
demu_poll_buffered_iopage(void)
{
    if (demu_state.seq != DEMU_SEQ_INITIALIZED)
        return;

    for (;;) {
        unsigned int    read_pointer;
        unsigned int    write_pointer;
        
        read_pointer = demu_state.buffered_iopage->read_pointer;
        write_pointer = demu_state.buffered_iopage->write_pointer;
        mb();

        if (read_pointer == write_pointer)
            break;

        while (read_pointer != write_pointer) {
            unsigned int    slot;
            buf_ioreq_t     *buf_ioreq;
            ioreq_t         ioreq;

            slot = read_pointer % IOREQ_BUFFER_SLOT_NUM;

            buf_ioreq = &demu_state.buffered_iopage->buf_ioreq[slot];

            ioreq.size = 1UL << buf_ioreq->size;
            ioreq.count = 1;
            ioreq.addr = buf_ioreq->addr;
            ioreq.data = buf_ioreq->data;
            ioreq.state = STATE_IOREQ_READY;
            ioreq.dir = buf_ioreq->dir;
            ioreq.df = 1;
            ioreq.type = buf_ioreq->type;
            ioreq.data_is_ptr = 0;

            read_pointer++;

            if (ioreq.size == 8) {
                slot = read_pointer % IOREQ_BUFFER_SLOT_NUM;
                buf_ioreq = &demu_state.buffered_iopage->buf_ioreq[slot];

                ioreq.data |= ((uint64_t)buf_ioreq->data) << 32;

                read_pointer++;
            }

            demu_handle_ioreq(&ioreq);
            mb();
        }

        demu_state.buffered_iopage->read_pointer = read_pointer;
        mb();
    }
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

    mb();

    ioreq->state = STATE_IOREQ_INPROCESS;

    demu_handle_ioreq(ioreq);
    mb();

    ioreq->state = STATE_IORESP_READY;
    mb();

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

    if (port == demu_state.buf_ioreq_local_port) {
        xenevtchn_unmask(demu_state.xeh, port);
        demu_poll_buffered_iopage();
    } else {
        for (i = 0; i < demu_state.vcpus; i++) {
            if (port == demu_state.ioreq_local_port[i]) {
                xenevtchn_unmask(demu_state.xeh, port);
                demu_poll_shared_iopage(i);
            }
        }
    }
}

int
main(int argc, char **argv, char **envp)
{
    char            *domain_str;
    char            *device_str;
    int             index;
    char            *end;
    domid_t         domid;
    sigset_t        block;
    int             rc;
    struct pollfd pfd;

    prog = basename(argv[0]);

    domain_str = NULL;
    device_str = NULL;

    for (;;) {
        char    c;

        c = getopt_long(argc, argv, "", demu_option, &index);
        if (c == -1)
            break;

        if (c != 0) {
            usage();
            /*NOTREACHED*/
        }

        DBG("--%s = '%s'\n", demu_option[index].name, optarg);

        switch (index) {
        case DEMU_OPT_DOMAIN:
            domain_str = optarg;
            break;

        case DEMU_OPT_DEVICE:
            device_str = optarg;
            break;

        default:
            assert(FALSE);
            break;
        }
    }

    if (domain_str == NULL) {
        usage();
        /*NOTREACHED*/
    }

    domid = (domid_t)strtol(domain_str, &end, 0);
    if (*end != '\0') {
        fprintf(stderr, "invalid domain '%s'\n", domain_str);
        exit(1);
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

    rc = demu_initialize(domid, device_str);
    if (rc < 0) {
        demu_teardown();
        exit(1);
    }

    pfd.fd = xenevtchn_fd(demu_state.xeh);
    pfd.events = POLLIN | POLLERR | POLLHUP;
    pfd.revents = 0;

    for (;;) {
        rc = poll(&pfd, 1, 5000);

        if (rc > 0 && pfd.revents & POLLIN)
            demu_poll_iopages();

        if (rc < 0 && errno != EINTR)
            break;
    }

    demu_teardown();

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

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

#include <locale.h>

#include <xenctrl.h>
#include <xen/hvm/ioreq.h>

#include "debug.h"
#include "device.h"
#include "pci.h"

#define FALSE 0

#define mb() asm volatile ("" : : : "memory")

enum {
    DEMU_OPT_DOMAIN,
    DEMU_OPT_VCPUS,
    DEMU_OPT_BUS,
    DEMU_OPT_DEVICE,
    DEMU_OPT_FUNCTION,
    DEMU_NR_OPTS
    };

static struct option demu_option[] = {
    {"domain", 1, NULL, 0},
    {"vcpus", 1, NULL, 0},
    {"bus", 1, NULL, 0},
    {"device", 1, NULL, 0},
    {"function", 1, NULL, 0},
    {NULL, 0, NULL, 0}
};

static const char *demu_option_text[] = {
    "<domid>",
    "<vcpu count>",
    "<bus>",
    "<device>",
    "<function>",
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
    DEMU_SEQ_INTERFACE_OPEN,
    DEMU_SEQ_SERVER_REGISTERED,
    DEMU_SEQ_SHARED_IOPAGE_MAPPED,
    DEMU_SEQ_BUFFERED_IOPAGE_MAPPED,
    DEMU_SEQ_PORT_ARRAY_ALLOCATED,
    DEMU_SEQ_EVTCHN_OPEN,
    DEMU_SEQ_PORTS_BOUND,
    DEMU_SEQ_BUF_PORT_BOUND,
    DEMU_SEQ_INITIALIZED,
    DEMU_NR_SEQS
} demu_seq_t;

typedef struct demu_state {
    demu_seq_t          seq;
    xc_interface        *xch;
    xc_interface        *xceh;
    domid_t             domid;
    unsigned int        vcpus;
    ioservid_t          ioservid;
    shared_iopage_t     *iopage;
    evtchn_port_t       *ioreq_local_port;
    buffered_iopage_t   *buffered_iopage;
    evtchn_port_t       buf_ioreq_port;
    evtchn_port_t       buf_ioreq_local_port;
} demu_state_t;

static demu_state_t demu_state;

static void
handle_pio(ioreq_t *ioreq)
{
    if (ioreq->dir == IOREQ_READ) {
        if (!ioreq->data_is_ptr) {
            ioreq->data = (uint64_t)pci_bar_read(0, ioreq->addr, ioreq->size);
        } else {
            assert(FALSE);
        }
    } else if (ioreq->dir == IOREQ_WRITE) {
        if (!ioreq->data_is_ptr) {
            pci_bar_write(0, ioreq->addr, ioreq->size, (uint32_t)ioreq->data);
        } else {
            assert(FALSE);
        }
    }
}

static void
handle_copy(ioreq_t *ioreq)
{
    if (ioreq->dir == IOREQ_READ) {
        if (!ioreq->data_is_ptr) {
            ioreq->data = (uint64_t)pci_bar_read(1, ioreq->addr, ioreq->size);
        } else {
            assert(FALSE);
        }
    } else if (ioreq->dir == IOREQ_WRITE) {
        if (!ioreq->data_is_ptr) {
            pci_bar_write(1, ioreq->addr, ioreq->size, (uint32_t)ioreq->data);
        } else {
            assert(FALSE);
        }
    }
}

static void
handle_pci_config(ioreq_t *ioreq)
{
    if (ioreq->dir == IOREQ_READ) {
        if (!ioreq->data_is_ptr) {
            ioreq->data = (uint32_t)pci_config_read(ioreq->addr, ioreq->size);
        } else {
            assert(FALSE);
        }
    } else if (ioreq->dir == IOREQ_WRITE) {
        if (!ioreq->data_is_ptr) {
            pci_config_write(ioreq->addr, ioreq->size, (uint32_t)ioreq->data);
        } else {
            assert(FALSE);
        }
    }
}

static void
handle_ioreq(ioreq_t *ioreq)
{
    switch (ioreq->type) {
    case IOREQ_TYPE_PIO:
        handle_pio(ioreq);
        break;

    case IOREQ_TYPE_COPY:
        handle_copy(ioreq);
        break;

    case IOREQ_TYPE_PCI_CONFIG:
        handle_pci_config(ioreq);
        break;

    case IOREQ_TYPE_TIMEOFFSET:
        break;

    case IOREQ_TYPE_INVALIDATE:
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
    case DEMU_SEQ_INTERFACE_OPEN:
        DBG(">INTERFACE_OPEN\n");
        break;

    case DEMU_SEQ_SERVER_REGISTERED:
        DBG(">SERVER_REGISTERED\n");
        DBG("ioservid = %u\n", demu_state.ioservid);
        break;

    case DEMU_SEQ_SHARED_IOPAGE_MAPPED:
        DBG(">SHARED_IOPAGE_MAPPED\n");
        DBG("iopage = %p\n", demu_state.iopage);
        break;

    case DEMU_SEQ_BUFFERED_IOPAGE_MAPPED:
        DBG(">BUFFERED_IOPAGE_MAPPED\n");
        DBG("buffered_iopage = %p\n", demu_state.buffered_iopage);
        break;

    case DEMU_SEQ_PORT_ARRAY_ALLOCATED:
        DBG(">PORT_ARRAY_ALLOCATED\n");
        break;

    case DEMU_SEQ_EVTCHN_OPEN:
        DBG(">EVTCHN_OPEN\n");
        break;

    case DEMU_SEQ_PORTS_BOUND: {
        int i;

        DBG(">EVTCHN_PORTS_BOUND\n");

        for (i = 0; i < demu_state.vcpus; i++)
            DBG("VCPU%d: %u -> %u\n", i,
                demu_state.iopage->vcpu_ioreq[i].vp_eport,
                demu_state.ioreq_local_port[i]);

        break;
    }

    case DEMU_SEQ_BUF_PORT_BOUND:
        DBG(">EVTCHN_BUF_PORT_BOUND\n");

        DBG("%u -> %u\n",
            demu_state.buf_ioreq_port,
            demu_state.buf_ioreq_local_port);
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
        device_teardown();

        demu_state.seq = DEMU_SEQ_PORTS_BOUND;
    }

    if (demu_state.seq >= DEMU_SEQ_PORTS_BOUND) {
        DBG("<EVTCHN_BUF_PORT_BOUND\n");
        evtchn_port_t   port;

        port = demu_state.buf_ioreq_local_port;

        DBG("%u\n", port);
        (void) xc_evtchn_unbind(demu_state.xceh, port);

        demu_state.seq = DEMU_SEQ_PORTS_BOUND;
    }

    if (demu_state.seq >= DEMU_SEQ_PORTS_BOUND) {
        DBG("<EVTCHN_PORTS_BOUND\n");

        demu_state.seq = DEMU_SEQ_EVTCHN_OPEN;
    }

    if (demu_state.seq >= DEMU_SEQ_EVTCHN_OPEN) {
        int i;

        DBG("<EVTCHN_OPEN\n");

        for (i = 0; i < demu_state.vcpus; i++) {
            evtchn_port_t   port;

            port = demu_state.ioreq_local_port[i];

            if (port >= 0) {
                DBG("VCPU%d: %u\n", i, port);
                (void) xc_evtchn_unbind(demu_state.xceh, port);
            }
        }

        xc_evtchn_close(demu_state.xceh);

        demu_state.seq = DEMU_SEQ_PORT_ARRAY_ALLOCATED;
    }

    if (demu_state.seq >= DEMU_SEQ_PORT_ARRAY_ALLOCATED) {
        DBG("<PORT_ARRAY_ALLOCATED\n");

        free(demu_state.ioreq_local_port);

        demu_state.seq = DEMU_SEQ_BUFFERED_IOPAGE_MAPPED;
    }

    if (demu_state.seq >= DEMU_SEQ_BUFFERED_IOPAGE_MAPPED) {
        DBG("<BUFFERED_IOPAGE_MAPPED\n");

        munmap(demu_state.buffered_iopage, XC_PAGE_SIZE);

        demu_state.seq = DEMU_SEQ_SHARED_IOPAGE_MAPPED;
    }

    if (demu_state.seq >= DEMU_SEQ_SHARED_IOPAGE_MAPPED) {
        DBG("<SHARED_IOPAGE_MAPPED\n");

        munmap(demu_state.iopage, XC_PAGE_SIZE);

        demu_state.seq = DEMU_SEQ_SERVER_REGISTERED;
    }

    if (demu_state.seq >= DEMU_SEQ_SERVER_REGISTERED) {
        DBG("<SERVER_REGISTERED\n");

        (void) xc_hvm_destroy_ioreq_server(demu_state.xch,
                                           demu_state.domid,
                                           demu_state.ioservid);
        demu_state.seq = DEMU_SEQ_INTERFACE_OPEN;
    }

    if (demu_state.seq >= DEMU_SEQ_INTERFACE_OPEN) {
        DBG("<INTERFACE_OPEN\n");

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

static struct sigaction sigusr1_handler;

static void
demu_sigusr1(int num)
{
    DBG("%s\n", strsignal(num));

    sigaction(SIGHUP, &sigusr1_handler, NULL);

    pci_config_dump();
}

static int
demu_initialize(domid_t domid, unsigned int vcpus, unsigned int bus,
                unsigned int device, unsigned int function)
{
    int             rc;
    xc_dominfo_t    dominfo;
    unsigned long   pfn;
    unsigned long   buf_pfn;
    evtchn_port_t   port;
    evtchn_port_t   buf_port;
    int             i;

    demu_state.domid = domid;
    demu_state.vcpus = vcpus;

    demu_state.xch = xc_interface_open(NULL, NULL, 0);
    if (demu_state.xch == NULL)
        goto fail1;

    demu_seq_next();

    rc = xc_domain_getinfo(demu_state.xch, demu_state.domid, 1, &dominfo);
    if (rc < 0 || dominfo.domid != demu_state.domid)
        goto fail2;

    rc = xc_hvm_create_ioreq_server(demu_state.xch, demu_state.domid, &demu_state.ioservid);
    if (rc < 0)
        goto fail3;
    
    demu_seq_next();

    rc = xc_hvm_get_ioreq_server_info(demu_state.xch, demu_state.domid,
                                      demu_state.ioservid, &pfn, &buf_pfn, &buf_port);
    if (rc < 0)
        goto fail4;

    demu_state.iopage = xc_map_foreign_range(demu_state.xch,
                                             demu_state.domid,
                                             XC_PAGE_SIZE,
                                             PROT_READ | PROT_WRITE,
                                             pfn);
    if (demu_state.iopage == NULL)
        goto fail5;

    demu_seq_next();

    demu_state.buffered_iopage = xc_map_foreign_range(demu_state.xch,
                                                      demu_state.domid,
                                                      XC_PAGE_SIZE,
                                                      PROT_READ | PROT_WRITE,
                                                      buf_pfn);
    if (demu_state.buffered_iopage == NULL)
        goto fail6;

    demu_seq_next();

    demu_state.ioreq_local_port = malloc(sizeof (evtchn_port_t) *
                                         demu_state.vcpus);
    if (demu_state.ioreq_local_port == NULL)
        goto fail7;

    for (i = 0; i < demu_state.vcpus; i++)
        demu_state.ioreq_local_port[i] = -1;

    demu_seq_next();

    demu_state.xceh = xc_evtchn_open(NULL, 0);
    if (demu_state.xceh == NULL)
        goto fail8;

    demu_seq_next();

    for (i = 0; i < demu_state.vcpus; i++) {
        port = demu_state.iopage->vcpu_ioreq[i].vp_eport;

        rc = xc_evtchn_bind_interdomain(demu_state.xceh, demu_state.domid,
                                        port);
        if (rc < 0)
            goto fail9;

        demu_state.ioreq_local_port[i] = rc;
    }

    demu_seq_next();

    rc = xc_evtchn_bind_interdomain(demu_state.xceh, demu_state.domid,
                                    buf_port);
    if (rc < 0)
        goto fail10;

    demu_state.buf_ioreq_local_port = rc;

    demu_seq_next();

    rc = device_initialize(demu_state.xch, demu_state.domid,
                           demu_state.ioservid, bus, device, function);
    if (rc < 0)
        goto fail11;

    demu_seq_next();

    assert(demu_state.seq == DEMU_SEQ_INITIALIZED);
    return 0;

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

            handle_ioreq(&ioreq);
            mb();
        }

        demu_state.buffered_iopage->read_pointer = read_pointer;
        mb();
    }
}

static void
demu_poll_iopage(unsigned int i)
{
    ioreq_t         *ioreq;

    if (demu_state.seq != DEMU_SEQ_INITIALIZED)
        return;

    ioreq = &demu_state.iopage->vcpu_ioreq[i];
    if (ioreq->state != STATE_IOREQ_READY) {
        fprintf(stderr, "IO request not ready\n");
        return;
    }
    mb();

    ioreq->state = STATE_IOREQ_INPROCESS;

    handle_ioreq(ioreq);
    mb();

    ioreq->state = STATE_IORESP_READY;
    mb();

    xc_evtchn_notify(demu_state.xceh, demu_state.ioreq_local_port[i]);
}

static void
demu_poll_iopages(void)
{
    evtchn_port_t   port;
    int             i;

    if (demu_state.seq != DEMU_SEQ_INITIALIZED)
        return;

    port = xc_evtchn_pending(demu_state.xceh);
    if (port < 0)
        return;

    if (port == demu_state.buf_ioreq_local_port) {
        xc_evtchn_unmask(demu_state.xceh, port);
        demu_poll_buffered_iopage();
    } else {
        for (i = 0; i < demu_state.vcpus; i++) {
            if (port == demu_state.ioreq_local_port[i]) {
                xc_evtchn_unmask(demu_state.xceh, port);
                demu_poll_iopage(i);
            }
        }
    }
}

int
main(int argc, char **argv, char **envp)
{
    char            *domain_str;
    char            *vcpus_str;
    char            *bus_str;
    char            *device_str;
    char            *function_str;
    int             index;
    char            *end;
    domid_t         domid;
    unsigned int    vcpus;
    unsigned int    bus;
    unsigned int    device;
    unsigned int    function;
    sigset_t        block;
    struct pollfd   pfd;
    int             rc;

    prog = basename(argv[0]);

    domain_str = NULL;
    vcpus_str = NULL;
    bus_str = NULL;
    device_str = NULL;
    function_str = NULL;

    for (;;) {
        char    c;

        c = getopt_long(argc, argv, "", demu_option, &index);
        if (c == -1)
            break;

        DBG("--%s = '%s'\n", demu_option[index].name, optarg);

        assert(c == 0);
        switch (index) {
        case DEMU_OPT_DOMAIN:
            domain_str = optarg;
            break;

        case DEMU_OPT_VCPUS:
            vcpus_str = optarg;
            break;

        case DEMU_OPT_BUS:
            bus_str = optarg;
            break;

        case DEMU_OPT_DEVICE:
            device_str = optarg;
            break;

        case DEMU_OPT_FUNCTION:
            function_str = optarg;
            break;

        default:
            assert(FALSE);
            break;
        }
    }

    if (domain_str == NULL ||
        vcpus_str == NULL ||
        bus_str == NULL ||
        device_str == NULL ||
        function_str == NULL) {
        usage();
        /*NOTREACHED*/
    }

    domid = (domid_t)strtol(domain_str, &end, 0);
    if (*end != '\0') {
        fprintf(stderr, "invalid domain '%s'\n", domain_str);
        exit(1);
    }

    vcpus = (unsigned int)strtol(vcpus_str, &end, 0);
    if (*end != '\0') {
        fprintf(stderr, "invalid vcpu count '%s'\n", vcpus_str);
        exit(1);
    }

    bus = (unsigned int)strtol(bus_str, &end, 0);
    if (*end != '\0') {
        fprintf(stderr, "invalid bus '%s'\n", bus_str);
        exit(1);
    }

    device = (unsigned int)strtol(device_str, &end, 0);
    if (*end != '\0') {
        fprintf(stderr, "invalid vcpu count '%s'\n", device_str);
        exit(1);
    }

    function = (unsigned int)strtol(function_str, &end, 0);
    if (*end != '\0') {
        fprintf(stderr, "invalid vcpu count '%s'\n", function_str);
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

    memset(&sigusr1_handler, 0, sizeof (struct sigaction));
    sigusr1_handler.sa_handler = demu_sigusr1;

    sigaction(SIGUSR1, &sigusr1_handler, NULL);
    sigdelset(&block, SIGUSR1);

    sigprocmask(SIG_BLOCK, &block, NULL);

    rc = demu_initialize(domid, vcpus, bus, device, function);
    if (rc < 0) {
        demu_teardown();
        exit(1);
    }

    pfd.fd = xc_evtchn_fd(demu_state.xceh);
    pfd.events = POLLIN | POLLERR | POLLHUP;
    pfd.revents = 0;

    for (;;) {
        rc = poll(&pfd, 1, 5000);

        if (rc > 0 && pfd.revents & POLLIN)
            demu_poll_iopages();

        if (rc < 0 && errno != EINTR)
            break;
    }

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

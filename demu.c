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

#include <locale.h>

#include <xenctrl.h>
#include <xen/hvm/ioreq.h>

#include <rfb/rfb.h>
#include <rfb/keysym.h>

#include "debug.h"
#include "mapcache.h"
#include "ps2.h"
#include "kbd.h"
#include "mouse.h"
#include "vga.h"
#include "pci.h"
#include "surface.h"
#include "demu.h"

#define mb() asm volatile ("" : : : "memory")

#define __min(_x, _y) (((_x) <= (_y)) ? (_x) : (_y))
#define __max(_x, _y) (((_x) > (_y)) ? (_x) : (_y))

#define DEMU_VRAM_SIZE  0x01000000
#define DEMU_ROM_FILE   "vgabios-stdvga.bin"
#define DEMU_KEYMAP     "en-us"

enum {
    DEMU_OPT_DOMAIN,
    DEMU_OPT_DEVICE,
    DEMU_OPT_ROM,
    DEMU_OPT_KEYMAP,
    DEMU_NR_OPTS
    };

static struct option demu_option[] = {
    {"domain", 1, NULL, 0},
    {"device", 1, NULL, 0},
    {"rom", 1, NULL, 0},
    {"keymap", 1, NULL, 0},
    {NULL, 0, NULL, 0}
};

static const char *demu_option_text[] = {
    "<domid>",
    "<device>",
    "<rom image>",
    "<map>",
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
    DEMU_SEQ_SERVER_ENABLED,
    DEMU_SEQ_PORT_ARRAY_ALLOCATED,
    DEMU_SEQ_EVTCHN_OPEN,
    DEMU_SEQ_PORTS_BOUND,
    DEMU_SEQ_BUF_PORT_BOUND,
    DEMU_SEQ_CMD_INITIALIZED,
    DEMU_SEQ_KBD_INITIALIZED,
    DEMU_SEQ_MOUSE_INITIALIZED,
    DEMU_SEQ_VNC_INITIALIZED,
    DEMU_SEQ_VGA_INITIALIZED,
    DEMU_SEQ_PS2_INITIALIZED,
    DEMU_SEQ_SURFACE_INITIALIZED,
    DEMU_SEQ_THREAD_INITIALIZED,
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

struct demu_kbd_cmd {
    char        cmd;
    rfbKeySym   sym;
    int         down;
};

struct demu_mouse_cmd {
    char    cmd;
    int     x;
    int     y;
    int     buttons;
};

#define MAX_CMDLEN  (__max(sizeof (struct demu_mouse_cmd), \
                           sizeof (struct demu_kbd_cmd)))

typedef struct demu_state {
    demu_seq_t          seq;
    timer_t             timer_id;
    void                (*tick)(void);
    int                 cmd[2];
    xc_interface        *xch;
    xc_interface        *xceh;
    domid_t             domid;
    unsigned int        vcpus;
    ioservid_t          ioservid;
    shared_iopage_t     *shared_iopage;
    evtchn_port_t       *ioreq_local_port;
    buffered_iopage_t   *buffered_iopage;
    evtchn_port_t       buf_ioreq_port;
    evtchn_port_t       buf_ioreq_local_port;
    uint32_t            width;
    uint32_t            height;
    uint32_t            depth;
    uint8_t             *default_framebuffer;
    uint8_t             *framebuffer;
    rfbScreenInfoPtr    screen;
    demu_space_t	    *memory;
    demu_space_t        *port;
    demu_space_t        *pci_config;
    pthread_t           thread;
} demu_state_t;

#define DEMU_VNC_DEFAULT_WIDTH  640
#define DEMU_VNC_DEFAULT_HEIGHT 480
#define DEMU_VNC_DEFAULT_DEPTH  4

static demu_state_t demu_state;

void
demu_set_irq(int irq, int level)
{
    xc_hvm_set_isa_irq_level(demu_state.xch, demu_state.domid, irq, level);
}

void *
demu_map_guest_pages(xen_pfn_t pfn[], unsigned int n, int populate)
{
    void *ptr;

    if (populate) {
        int rc;

        rc = xc_domain_populate_physmap_exact(demu_state.xch, demu_state.domid,
                                              n, 0, 0, pfn);
        if (rc < 0)
            goto fail1;
    }

    ptr = xc_map_foreign_pages(demu_state.xch, demu_state.domid,
                               PROT_READ | PROT_WRITE,
                               pfn, n);
    if (ptr == NULL)
        goto fail2;

    if (populate)
        memset(ptr, 0, n * TARGET_PAGE_SIZE);
    
    return ptr;

fail2:
    DBG("fail2\n");

    if (populate)
        (void) xc_domain_decrease_reservation(demu_state.xch, demu_state.domid,
                                              n, 0, pfn);
    
fail1:
    DBG("fail1\n");

    warn("fail");
    return NULL;
}

void *
demu_map_guest_range(uint64_t addr, uint64_t size, int populate)
{
    xen_pfn_t   *pfn;
    int         i, n;
    void        *ptr;

    DBG("%"PRIx64"+%"PRIx64" %s\n", addr, size, (populate) ? "[POPULATE]" : "");

    size = P2ROUNDUP(size, TARGET_PAGE_SIZE);

    n = size >> TARGET_PAGE_SHIFT;
    pfn = malloc(sizeof (xen_pfn_t) * n);

    if (pfn == NULL)
        goto fail1;

    for (i = 0; i < n; i++)
        pfn[i] = (addr >> TARGET_PAGE_SHIFT) + i;
    
    ptr = demu_map_guest_pages(pfn, n, populate);
    if (ptr == NULL)
        goto fail2;
    
    free(pfn);
    return ptr;
    
fail2:
    DBG("fail2\n");
    
    free(pfn);
    
fail1:
    DBG("fail1\n");

    warn("fail");
    return NULL;
}

void
demu_unmap_guest_pages(void *ptr, xen_pfn_t pfn[], unsigned int n, int depopulate)
{
    munmap(ptr, TARGET_PAGE_SIZE * n);
    if (depopulate)
        (void) xc_domain_decrease_reservation(demu_state.xch, demu_state.domid,
                                              n, 0, pfn);
}

int
demu_unmap_guest_range(void *ptr, uint64_t addr, uint64_t size, int depopulate)
{
    xen_pfn_t   *pfn;
    int         i, n;

    DBG("%"PRIx64"+%"PRIx64" %s\n", addr, size, (depopulate) ? "[DEPOPULATE]" : "");

    size = P2ROUNDUP(size, TARGET_PAGE_SIZE);

    n = size >> TARGET_PAGE_SHIFT;
    pfn = malloc(sizeof (xen_pfn_t) * n);

    if (pfn == NULL)
        goto fail1;

    for (i = 0; i < n; i++)
        pfn[i] = (addr >> TARGET_PAGE_SHIFT) + i;
    
    demu_unmap_guest_pages(ptr, pfn, n, depopulate);
    
    free(pfn);
    return 0;
    
fail1:
    DBG("fail1\n");

    warn("fail");
    return -1;
}

void
demu_set_guest_dirty_page(xen_pfn_t pfn)
{
    (void) xc_hvm_modified_memory(demu_state.xch, demu_state.domid,
                                  pfn, 1);
}

void
demu_track_dirty_vram(xen_pfn_t pfn, int n, unsigned long *bitmap)
{
    int rc;

    rc = xc_hvm_track_dirty_vram(demu_state.xch, demu_state.domid,
                                 pfn, n, bitmap);
    if (rc < 0 && bitmap != NULL)
        memset(bitmap, 0, n / 8);
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
demu_find_pci_config_space(uint32_t sbdf)
{
    demu_space_t    *space;

    space = demu_find_space(demu_state.pci_config, sbdf);

    if (space == NULL)
        DBG("failed to find space for 0x%x\n", sbdf);

    return space;
}

static demu_space_t *
demu_find_port_space(uint64_t addr)
{
    demu_space_t    *space;

    space = demu_find_space(demu_state.port, addr);

    if (space == NULL)
        DBG("failed to find space for 0x%"PRIx64"\n", addr);

    return space;
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
demu_register_pci_config_space(uint8_t bus, uint8_t device, uint8_t function,
                               const io_ops_t *ops, void *priv)
{
    uint32_t    sbdf;
    int         rc;

    DBG("%02x:%02x:%02x\n", bus, device, function);

    sbdf = (bus << 8) | (device << 3) | function;

    rc = demu_register_space(&demu_state.pci_config, sbdf, sbdf, ops, priv);
    if (rc < 0)
        goto fail1;

    rc = xc_hvm_map_pcidev_to_ioreq_server(demu_state.xch, demu_state.domid, demu_state.ioservid,
                                           0, bus, device, function);
    if (rc < 0)
        goto fail2;

    return 0;

fail2:
    DBG("fail2\n");

    demu_deregister_space(&demu_state.pci_config, sbdf, NULL);

fail1:
    DBG("fail1\n");

    warn("fail");
    return -1;
}

int
demu_register_port_space(uint64_t start, uint64_t size, const io_ops_t *ops, void *priv)
{
    int rc;

    DBG("%"PRIx64" - %"PRIx64"\n", start, start + size - 1);

    rc = demu_register_space(&demu_state.port, start, start + size - 1, ops, priv);
    if (rc < 0)
        goto fail1;

    rc = xc_hvm_map_io_range_to_ioreq_server(demu_state.xch, demu_state.domid, demu_state.ioservid,
                                             0, start, start + size - 1);
    if (rc < 0)
        goto fail2;

    return 0;

fail2:
    DBG("fail2\n");

    demu_deregister_space(&demu_state.port, start, NULL);

fail1:
    DBG("fail1\n");

    warn("fail");
    return -1;
}

int
demu_register_memory_space(uint64_t start, uint64_t size, const io_ops_t *ops, void *priv)
{
    int rc;

    DBG("%"PRIx64" - %"PRIx64"\n", start, start + size - 1);

    rc = demu_register_space(&demu_state.memory, start, start + size - 1, ops, priv);
    if (rc < 0)
        goto fail1;

    rc = xc_hvm_map_io_range_to_ioreq_server(demu_state.xch, demu_state.domid, demu_state.ioservid,
                                             1, start, start + size - 1);
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
demu_deregister_pci_config_space(uint8_t bus, uint8_t device, uint8_t function)
{
    uint32_t    sbdf;

    DBG("%02x:%02x:%02x\n", bus, device, function);

    sbdf = (bus << 8) | (device << 3) | function;

    demu_deregister_space(&demu_state.pci_config, sbdf, NULL);

    xc_hvm_unmap_pcidev_from_ioreq_server(demu_state.xch, demu_state.domid, demu_state.ioservid,
                                          0, bus, device, function);
}

void
demu_deregister_port_space(uint64_t start)
{
    uint64_t end;

    DBG("%"PRIx64"\n", start);

    demu_deregister_space(&demu_state.port, start, &end);

    xc_hvm_unmap_io_range_from_ioreq_server(demu_state.xch, demu_state.domid, demu_state.ioservid,
                                            0, start, end);
}

void
demu_deregister_memory_space(uint64_t start)
{
    uint64_t end;

    DBG("%"PRIx64"\n", start);

    demu_deregister_space(&demu_state.memory, start, &end);

    xc_hvm_unmap_io_range_from_ioreq_server(demu_state.xch, demu_state.domid, demu_state.ioservid,
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
                DEMU_IO_READ(space->ops->readb, space->priv, addr, 1, 4, val);
            else
                DEMU_IO_READ(space->ops->readw, space->priv, addr, 2, 2, val);
        } else {
            DEMU_IO_READ(space->ops->readl, space->priv, addr, 4, 1, val);
        }
        break;

    case 8:
        if (space->ops->readl == NULL) {
            if (space->ops->readw == NULL)
                DEMU_IO_READ(space->ops->readb, space->priv, addr, 1, 8, val);
            else
                DEMU_IO_READ(space->ops->readw, space->priv, addr, 2, 4, val);
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
demu_io_write(demu_space_t *space, uint64_t addr, uint64_t size, uint64_t val)
{
    switch (size) {
    case 1:
        space->ops->writeb(space->priv, addr, val);
        break;

    case 2:
        if (space->ops->writew == NULL)
            DEMU_IO_WRITE(space->ops->writeb, space->priv, addr, 1, 2, val);
        else
            DEMU_IO_WRITE(space->ops->writew, space->priv, addr, 2, 1, val);
        break;

    case 4:
        if (space->ops->writel == NULL) {
            if (space->ops->writew == NULL)
                DEMU_IO_WRITE(space->ops->writeb, space->priv, addr, 1, 4, val);
            else
                DEMU_IO_WRITE(space->ops->writew, space->priv, addr, 2, 2, val);
        } else {
            DEMU_IO_WRITE(space->ops->writel, space->priv, addr, 4, 1, val);
        }
        break;

    case 8:
        if (space->ops->writel == NULL) {
            if (space->ops->writew == NULL)
                DEMU_IO_WRITE(space->ops->writeb, space->priv, addr, 1, 8, val);
            else
                DEMU_IO_WRITE(space->ops->writew, space->priv, addr, 2, 4, val);
        } else {
            DEMU_IO_WRITE(space->ops->writel, space->priv, addr, 4, 2, val);
        }
        break;

    default:
        break;
    }
}

static inline void
__copy_to_guest_memory(uint64_t addr, uint64_t size, uint8_t *src)
{
    xen_pfn_t       pfn = addr >> TARGET_PAGE_SHIFT;
    uint64_t        offset = addr & (TARGET_PAGE_SIZE - 1);

    while (size != 0) {
        uint8_t     *dst;
        uint64_t    chunk;

        chunk = __min(size, TARGET_PAGE_SIZE - offset);

        dst = mapcache_lookup(pfn);
        if (dst == NULL)
            goto fail1;

        dst += offset;

        memcpy(dst, src, chunk);

        src += chunk;
        size -= chunk;

        pfn++;
        offset = 0;
    }

    return;

fail1:
    DBG("fail1\n");
}

static inline void
__copy_from_guest_memory(uint64_t addr, uint64_t size, uint8_t *dst)
{
    xen_pfn_t       pfn = addr >> TARGET_PAGE_SHIFT;
    uint64_t        offset = addr & (TARGET_PAGE_SIZE - 1);

    while (size != 0) {
        uint8_t     *src;
        uint64_t    chunk;

        chunk = __min(size, TARGET_PAGE_SIZE - offset);

        src = mapcache_lookup(pfn);
        if (src == NULL)
            goto fail1;

        src += offset;

        memcpy(dst, src, chunk);

        dst += chunk;
        size -= chunk;

        pfn++;
        offset = 0;
    }

    return;

fail1:
    DBG("fail1\n");

    memset(dst, 0xff, size);
}

static void
demu_handle_io(demu_space_t *space, ioreq_t *ioreq, int is_mmio)
{
    if (space == NULL)
        goto fail1;

    if (ioreq->dir == IOREQ_READ) {
        if (!ioreq->data_is_ptr) {
            ioreq->data = demu_io_read(space, ioreq->addr, ioreq->size);
        } else {
            int i, sign;

            sign = ioreq->df ? -1 : 1;
            for (i = 0; i < ioreq->count; i++) {
                uint64_t    data;
                
                data = demu_io_read(space, ioreq->addr, ioreq->size);

                __copy_to_guest_memory(ioreq->data + (sign * i * ioreq->size),
                                       ioreq->size, (uint8_t *)&data);

                if (is_mmio)
                    ioreq->addr += sign * ioreq->size;
            }
        }
    } else if (ioreq->dir == IOREQ_WRITE) {
        if (!ioreq->data_is_ptr) {
            demu_io_write(space, ioreq->addr, ioreq->size, ioreq->data);
        } else {
            int i, sign;

            sign = ioreq->df ? -1 : 1;
            for (i = 0; i < ioreq->count; i++) {
                uint64_t    data;

                __copy_from_guest_memory(ioreq->data + (sign * i * ioreq->size),
                                         ioreq->size, (uint8_t *)&data);

                demu_io_write(space, ioreq->addr, ioreq->size, data);

                if (is_mmio)
                    ioreq->addr += sign * ioreq->size;
            }
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
    case IOREQ_TYPE_PIO:
        space = demu_find_port_space(ioreq->addr);
        demu_handle_io(space, ioreq, FALSE);
        break;

    case IOREQ_TYPE_COPY:
        space = demu_find_memory_space(ioreq->addr);
        demu_handle_io(space, ioreq, TRUE);
        break;

    case IOREQ_TYPE_PCI_CONFIG: {
        uint32_t    sbdf;

        sbdf = (uint32_t)(ioreq->addr >> 32);

        ioreq->addr &= 0xffffffff;

        space = demu_find_pci_config_space(sbdf);
        demu_handle_io(space, ioreq, FALSE);
        break;
    }
    case IOREQ_TYPE_TIMEOFFSET:
        break;

    case IOREQ_TYPE_INVALIDATE:
        mapcache_invalidate();
        break;

    default:
        DBG("UNKNOWN (%02x)", ioreq->type);
        break;
    }
}

static void demu_vnc_mouse(int buttonMask, int x, int y, rfbClientPtr client)
{
    struct demu_mouse_cmd   cmd;

    cmd.cmd = 'M';
    cmd.x = x;
    cmd.y = y;
    cmd.buttons = buttonMask;

    write(demu_state.cmd[1], &cmd, sizeof (cmd));

    rfbDefaultPtrAddEvent(buttonMask, x, y, client);
}

static void demu_vnc_key(rfbBool down, rfbKeySym sym, rfbClientPtr client)
{
    struct demu_kbd_cmd     cmd;

    cmd.cmd = 'K';
    cmd.sym = sym;
    cmd.down = down;

    write(demu_state.cmd[1], &cmd, sizeof (cmd));
}

static void demu_vnc_remove_client(rfbClientPtr client)
{
    DBG("\n");
}

static enum rfbNewClientAction demu_vnc_add_client(rfbClientPtr client)
{
    DBG("\n");
    client->clientGoneHook = demu_vnc_remove_client;

    return RFB_CLIENT_ACCEPT;
}

void
demu_new_framebuffer(uint32_t width, uint32_t height, uint32_t depth)
{
    rfbScreenInfoPtr    screen = demu_state.screen;

    if (demu_state.framebuffer != NULL)
        free(demu_state.framebuffer);

    demu_state.framebuffer = malloc(width * height * depth);

    if (demu_state.framebuffer == NULL) {
        DBG("allocation failed: using default framebuffer (%dx%dx%d)\n",
            DEMU_VNC_DEFAULT_WIDTH,
            DEMU_VNC_DEFAULT_HEIGHT,
            DEMU_VNC_DEFAULT_DEPTH);

        demu_state.width = DEMU_VNC_DEFAULT_WIDTH;
        demu_state.height = DEMU_VNC_DEFAULT_HEIGHT;
        demu_state.width = DEMU_VNC_DEFAULT_DEPTH;

        rfbNewFramebuffer(screen,
                          (char *)demu_state.default_framebuffer,
                          DEMU_VNC_DEFAULT_WIDTH,
                          DEMU_VNC_DEFAULT_HEIGHT,
                          8, 3,
                          DEMU_VNC_DEFAULT_DEPTH);
    } else {
        DBG("%dx%dx%d\n", width, height, depth);

        demu_state.width = width;
        demu_state.height = height;
        demu_state.width = depth;

        rfbNewFramebuffer(screen,
                          (char *)demu_state.framebuffer,
                          width,
                          height,
                          8, 3,
                          depth);
    }
}

uint8_t *
demu_get_framebuffer(void)
{
    return demu_state.framebuffer;
}   

void
demu_update_framebuffer(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    rfbScreenInfoPtr    screen = demu_state.screen; 

    rfbMarkRectAsModified(screen, x * 160, y, (x + width) * 160, y + height);
    //rfbMarkRectAsModified(screen, 0, 0, demu_state.width * 160, demu_state.height);
}   

static int
demu_vnc_initialize(void)
{
    uint8_t             *framebuffer;
    unsigned int        x, y;
    rfbScreenInfoPtr    screen;

    framebuffer = malloc(DEMU_VNC_DEFAULT_WIDTH *
                         DEMU_VNC_DEFAULT_HEIGHT *
                         DEMU_VNC_DEFAULT_DEPTH);
    if (framebuffer == NULL)
        goto fail1;

    for (y = 0; y < DEMU_VNC_DEFAULT_HEIGHT; y++) {
        for (x = 0; x < DEMU_VNC_DEFAULT_WIDTH; x++) {
            uint8_t *pixel = &framebuffer[(y * DEMU_VNC_DEFAULT_WIDTH + x) *
                                          DEMU_VNC_DEFAULT_DEPTH];
            pixel[0] = (y * 256) / DEMU_VNC_DEFAULT_HEIGHT; /* RED */
            pixel[1] = (y * 256) / DEMU_VNC_DEFAULT_HEIGHT; /* GREEN */
            pixel[2] = (y * 256) / DEMU_VNC_DEFAULT_HEIGHT; /* BLUE */
        }
    }   
   
    demu_state.default_framebuffer = framebuffer;

    screen = rfbGetScreen(NULL, NULL,
                          DEMU_VNC_DEFAULT_WIDTH,
                          DEMU_VNC_DEFAULT_HEIGHT,
                          8, 3,
                          DEMU_VNC_DEFAULT_DEPTH);
    if (screen == NULL)
        goto fail2;
 
    screen->frameBuffer = (char *)demu_state.default_framebuffer;
    screen->desktopName = "DEMU";
    screen->alwaysShared = TRUE;
    screen->autoPort = TRUE;
    screen->ptrAddEvent = demu_vnc_mouse;
    screen->kbdAddEvent = demu_vnc_key;
    screen->newClientHook = demu_vnc_add_client;

    rfbInitServer(screen);
    demu_state.screen = screen;
    return 0;

fail2:
    DBG("fail2\n");
    rfbScreenCleanup(screen);

fail1:
    DBG("fail1\n");

    warn("fail");
    return -1;
}

static void
demu_vnc_teardown(void)
{
    rfbScreenInfoPtr    screen = demu_state.screen;

    rfbShutdownServer(screen, TRUE);
    rfbScreenCleanup(screen);

    if (demu_state.framebuffer != NULL)
        free(demu_state.framebuffer);
 
    free(demu_state.default_framebuffer);
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
        DBG("shared_iopage = %p\n", demu_state.shared_iopage);
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

    case DEMU_SEQ_CMD_INITIALIZED:
        DBG(">CMD_INITIALIZED\n");
        break;

    case DEMU_SEQ_KBD_INITIALIZED:
        DBG(">KBD_INITIALIZED\n");
        break;

    case DEMU_SEQ_MOUSE_INITIALIZED:
        DBG(">MOUSE_INITIALIZED\n");
        break;

    case DEMU_SEQ_VNC_INITIALIZED:
        DBG(">VNC_INITIALIZED\n");
        break;

    case DEMU_SEQ_VGA_INITIALIZED:
        DBG(">VGA_INITIALIZED\n");
        break;

    case DEMU_SEQ_PS2_INITIALIZED:
        DBG(">PS2_INITIALIZED\n");
        break;

    case DEMU_SEQ_SURFACE_INITIALIZED:
        DBG(">SURFACE_INITIALIZED\n");
        break;

    case DEMU_SEQ_THREAD_INITIALIZED:
        DBG(">THREAD_INITIALIZED\n");
        break;

    case DEMU_SEQ_SERVER_ENABLED:
        DBG(">SERVER_ENABLED\n");
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

        demu_state.seq = DEMU_SEQ_THREAD_INITIALIZED;
    }

    if (demu_state.seq == DEMU_SEQ_THREAD_INITIALIZED) {
        DBG("<THREAD_INITIALIZED\n");
        pthread_cancel(demu_state.thread);
        pthread_join(demu_state.thread, NULL);

        demu_state.seq = DEMU_SEQ_SURFACE_INITIALIZED;
    }

    if (demu_state.seq == DEMU_SEQ_SURFACE_INITIALIZED) {
        DBG("<SURFACE_INITIALIZED\n");
        surface_teardown();

        demu_state.seq = DEMU_SEQ_PS2_INITIALIZED;
    }

    if (demu_state.seq == DEMU_SEQ_PS2_INITIALIZED) {
        DBG("<PS2_INITIALIZED\n");
        ps2_teardown();

        demu_state.seq = DEMU_SEQ_VGA_INITIALIZED;
    }

    if (demu_state.seq == DEMU_SEQ_VGA_INITIALIZED) {
        DBG("<VGA_INITIALIZED\n");
        vga_teardown();

        demu_state.seq = DEMU_SEQ_VNC_INITIALIZED;
    }

    if (demu_state.seq == DEMU_SEQ_VNC_INITIALIZED) {
        DBG("<VNC_INITIALIZED\n");
        demu_vnc_teardown();

        demu_state.seq = DEMU_SEQ_KBD_INITIALIZED;
    }

    if (demu_state.seq == DEMU_SEQ_MOUSE_INITIALIZED) {
        DBG("<MOUSE_INITIALIZED\n");
        mouse_teardown();

        demu_state.seq = DEMU_SEQ_KBD_INITIALIZED;
    }

    if (demu_state.seq == DEMU_SEQ_KBD_INITIALIZED) {
        DBG("<KBD_INITIALIZED\n");
        kbd_teardown();

        demu_state.seq = DEMU_SEQ_CMD_INITIALIZED;
    }

    if (demu_state.seq == DEMU_SEQ_CMD_INITIALIZED) {
        DBG("<CMD_INITIALIZED\n");

        close(demu_state.cmd[1]);
        close(demu_state.cmd[0]);

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

        demu_state.seq = DEMU_SEQ_SERVER_ENABLED;
    }

    if (demu_state.seq == DEMU_SEQ_SERVER_ENABLED) {
        DBG("<SERVER_ENABLED\n");
        (void) xc_hvm_set_ioreq_server_state(demu_state.xch,
                                             demu_state.domid,
                                             demu_state.ioservid,
                                             0);

        demu_state.seq = DEMU_SEQ_BUFFERED_IOPAGE_MAPPED;
    }

    if (demu_state.seq >= DEMU_SEQ_BUFFERED_IOPAGE_MAPPED) {
        DBG("<BUFFERED_IOPAGE_MAPPED\n");

        munmap(demu_state.buffered_iopage, XC_PAGE_SIZE);

        demu_state.seq = DEMU_SEQ_SHARED_IOPAGE_MAPPED;
    }

    if (demu_state.seq >= DEMU_SEQ_SHARED_IOPAGE_MAPPED) {
        DBG("<SHARED_IOPAGE_MAPPED\n");

        munmap(demu_state.shared_iopage, XC_PAGE_SIZE);

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

    pci_device_dump();
}

static struct sigaction sigrt_handler;

static void
demu_sigrt(int num, siginfo_t *si, void *arg)
{
    int     tfd;
    char    buf = 'T';

    sigaction(SIGRTMIN, &sigrt_handler, NULL);

    tfd = (intptr_t)si->si_value.sival_ptr;
    write(tfd, &buf, 1);
}

static void *
demu_thread(void *arg)
{
    DBG("---->\n");

    for (;;) {
        if (rfbIsActive(demu_state.screen)) {
            surface_refresh();
            rfbProcessEvents(demu_state.screen, -1);
        }
    }

    DBG("<----\n");
    return NULL;
}

static int
demu_initialize(domid_t domid, unsigned int bus, unsigned int device, unsigned int function, char *keymap, char *rom_file)
{
    int             rc;
    xc_dominfo_t    dominfo;
    unsigned long   pfn;
    unsigned long   buf_pfn;
    evtchn_port_t   port;
    evtchn_port_t   buf_port;
    int             i;

    demu_state.domid = domid;

    demu_state.xch = xc_interface_open(NULL, NULL, 0);
    if (demu_state.xch == NULL)
        goto fail1;

    demu_seq_next();

    rc = xc_domain_getinfo(demu_state.xch, demu_state.domid, 1, &dominfo);
    if (rc < 0 || dominfo.domid != demu_state.domid)
        goto fail2;

    demu_state.vcpus = dominfo.max_vcpu_id + 1;

    DBG("%d vCPU(s)\n", demu_state.vcpus);

    rc = xc_hvm_create_ioreq_server(demu_state.xch, demu_state.domid, 1,
                                    &demu_state.ioservid);
    if (rc < 0)
        goto fail3;
    
    demu_seq_next();

    rc = xc_hvm_get_ioreq_server_info(demu_state.xch, demu_state.domid,
                                      demu_state.ioservid, &pfn, &buf_pfn, &buf_port);
    if (rc < 0)
        goto fail4;

    demu_state.shared_iopage = xc_map_foreign_range(demu_state.xch,
                                                    demu_state.domid,
                                                    XC_PAGE_SIZE,
                                                    PROT_READ | PROT_WRITE,
                                                    pfn);
    if (demu_state.shared_iopage == NULL)
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

    rc = xc_hvm_set_ioreq_server_state(demu_state.xch,
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

    demu_state.xceh = xc_evtchn_open(NULL, 0);
    if (demu_state.xceh == NULL)
        goto fail9;

    demu_seq_next();

    for (i = 0; i < demu_state.vcpus; i++) {
        port = demu_state.shared_iopage->vcpu_ioreq[i].vp_eport;

        rc = xc_evtchn_bind_interdomain(demu_state.xceh, demu_state.domid,
                                        port);
        if (rc < 0)
            goto fail10;

        demu_state.ioreq_local_port[i] = rc;
    }

    demu_seq_next();

    rc = xc_evtchn_bind_interdomain(demu_state.xceh, demu_state.domid,
                                    buf_port);
    if (rc < 0)
        goto fail11;

    demu_state.buf_ioreq_local_port = rc;

    demu_seq_next();

    if (pipe(demu_state.cmd) < 0)
        goto fail12;

    demu_seq_next();

    rc = kbd_initialize((keymap) ? keymap : DEMU_KEYMAP);
    if (rc < 0)
        goto fail13;

    demu_seq_next();

    rc = mouse_initialize();
    if (rc < 0)
        goto fail14;

    demu_seq_next();

    rc = demu_vnc_initialize();
    if (rc < 0)
        goto fail15;

    demu_seq_next();

    rc = vga_initialize(bus, device, function,
                        DEMU_VRAM_SIZE,
                        (rom_file) ? rom_file : DEMU_ROM_FILE);
    if (rc < 0)
        goto fail16;

    demu_seq_next();

    rc = ps2_initialize();
    if (rc < 0)
        goto fail17;

    demu_seq_next();

    rc = surface_initialize();
    if (rc < 0)
        goto fail18;

    demu_seq_next();

    rc = pthread_create(&demu_state.thread, NULL, demu_thread, NULL);
    if (rc != 0)
        goto fail19;

    demu_seq_next();

    demu_seq_next();

    assert(demu_state.seq == DEMU_SEQ_INITIALIZED);
    return 0;

fail19:
    DBG("fail19\n");

fail18:
    DBG("fail18\n");

fail17:
    DBG("fail17\n");

fail16:
    DBG("fail16\n");

fail15:
    DBG("fail15\n");

fail14:
    DBG("fail14\n");

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

    ioreq = &demu_state.shared_iopage->vcpu_ioreq[i];
    if (ioreq->state != STATE_IOREQ_READY)
        return;

    mb();

    ioreq->state = STATE_IOREQ_INPROCESS;

    demu_handle_ioreq(ioreq);
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
                demu_poll_shared_iopage(i);
            }
        }
    }
}

static int
demu_timer_create(void)
{
    int                 pfd[2];
    timer_t             tid;
    struct sigevent     sigev;

    memset(pfd, 0, sizeof (pfd));
    if (pipe(pfd) < 0)
        goto fail1;

    sigev.sigev_notify = SIGEV_SIGNAL;
    sigev.sigev_signo = SIGRTMIN;
    sigev.sigev_value.sival_ptr = (void *)(intptr_t)pfd[1];

    if (timer_create(CLOCK_MONOTONIC, &sigev, &tid) < 0)
        goto fail2;

    demu_state.timer_id = tid;

    return pfd[0];

fail2:
    DBG("fail2\n");

    close(pfd[1]);
    close(pfd[0]);

fail1:
    DBG("fail1: %s\n", strerror(errno));

    return -1;
}

static void
demu_timer_destroy(void)
{
    timer_delete(demu_state.timer_id);
}

int
demu_timer_start(unsigned int period, void (*tick)(void))
{
    time_t              s;
    long                ns;
    struct itimerspec   it;

    s = period / 1000;
    ns = (period - (s * 1000)) * 1000000;

    it.it_interval.tv_sec = it.it_value.tv_sec = s;
    it.it_interval.tv_nsec = it.it_value.tv_nsec = ns;

    demu_state.tick = tick;

    if (timer_settime(demu_state.timer_id, 0, &it, NULL) < 0)
        goto fail1;

    DBG("done: %u ms\n", period);

    return 0;

fail1:
    DBG("fail1: %s\n", strerror(errno));

    return -1;
}

void
demu_timer_tick(void)
{
    if (demu_state.tick != NULL)
        demu_state.tick();  
}

int
demu_timer_stop(void)
{
    struct itimerspec   it;

    it.it_interval.tv_sec = it.it_value.tv_sec = 0;
    it.it_interval.tv_nsec = it.it_value.tv_nsec = 0;

    if (timer_settime(demu_state.timer_id, 0, &it, NULL) < 0)
        goto fail1;

    demu_state.tick = NULL;

    DBG("done\n");

    return 0;

fail1:
    DBG("fail1: %s\n", strerror(errno));

    return -1;
}

int
main(int argc, char **argv, char **envp)
{
    char            *domain_str;
    char            *device_str;
    char            *rom_str;
    char            *keymap_str;
    int             index;
    char            *end;
    domid_t         domid;
    unsigned int    device;
    sigset_t        block;
    int             efd;
    int             tfd;
    int             cfd;
    int             rc;

    prog = basename(argv[0]);

    domain_str = NULL;
    device_str = NULL;
    rom_str = NULL;
    keymap_str = NULL;

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

        case DEMU_OPT_ROM:
            rom_str = optarg;
            break;

        case DEMU_OPT_KEYMAP:
            keymap_str = optarg;
            break;

        default:
            assert(FALSE);
            break;
        }
    }

    if (domain_str == NULL ||
        device_str == NULL) {
        usage();
        /*NOTREACHED*/
    }

    domid = (domid_t)strtol(domain_str, &end, 0);
    if (*end != '\0') {
        fprintf(stderr, "invalid domain '%s'\n", domain_str);
        exit(1);
    }

    device = (unsigned int)strtol(device_str, &end, 0);
    if (*end != '\0') {
        fprintf(stderr, "invalid device '%s'\n", device_str);
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

    memset(&sigrt_handler, 0, sizeof (struct sigaction));
    sigrt_handler.sa_flags = SA_SIGINFO;
    sigrt_handler.sa_sigaction = demu_sigrt;

    sigaction(SIGRTMIN, &sigrt_handler, NULL);
    sigdelset(&block, SIGRTMIN);

    sigprocmask(SIG_BLOCK, &block, NULL);

    tfd = demu_timer_create();
    if (tfd < 0)
        exit(1);

    rc = demu_initialize(domid, 0, device, 0, keymap_str, rom_str);
    if (rc < 0) {
        demu_teardown();
        exit(1);
    }

    efd = xc_evtchn_fd(demu_state.xceh);

    cfd = demu_state.cmd[0];
        
    for (;;) {
        fd_set          rfds;
        fd_set          wfds;
        fd_set          xfds;
        int             nfds;
        struct timeval  tv;

        FD_ZERO(&rfds);
        FD_ZERO(&wfds);
        FD_ZERO(&xfds);

        FD_SET(tfd, &rfds);
        FD_SET(efd, &rfds);
        FD_SET(cfd, &rfds);

        tv.tv_sec = 0;
        tv.tv_usec = demu_state.screen->deferUpdateTime * 1000;

        nfds = __max(tfd, (__max(efd, cfd))) + 1;
        rc = select(nfds, &rfds, &wfds, &xfds, &tv);

        if (rc > 0) {
            if (FD_ISSET(efd, &rfds))
                demu_poll_iopages();

            if (FD_ISSET(tfd, &rfds)) {
                char    buf;

                (void) read(tfd, &buf, 1);
                demu_timer_tick();
            }

            if (FD_ISSET(cfd, &rfds)) {
                char    buf[MAX_CMDLEN];
                ssize_t len;

                len = read(cfd, buf, MAX_CMDLEN);
                if (len > 0) {
                    switch (buf[0]) {
                    case 'K': {
                        struct demu_kbd_cmd *cmd;

                        assert(len >= sizeof (struct demu_kbd_cmd));
                        cmd = (struct demu_kbd_cmd *)&buf[0];

                        kbd_event(cmd->sym, cmd->down);
                        break;
                    }
                    case 'M': {
                        struct demu_mouse_cmd *cmd;

                        assert(len >= sizeof (struct demu_mouse_cmd));
                        cmd = (struct demu_mouse_cmd *)&buf[0];

                        mouse_event(cmd->x, cmd->y, cmd->buttons);
                        break;
                    }
                    default:
                        assert(FALSE);
                        break;
                    }
                }
            }
        }

        if (rc < 0 && errno != EINTR)
            break;
    }

    demu_teardown();

    demu_timer_destroy();

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

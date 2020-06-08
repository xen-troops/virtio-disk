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
#include <string.h>
#include <assert.h>
#include <inttypes.h>

#include <sys/types.h>
#include <sys/mman.h>

#include <xenctrl.h>

#include "debug.h"
#include "demu.h"

#include "kvm/kvm.h"

/*static unsigned long count[MAX_DISK_IMAGES];*/

typedef struct mapcache_entry {
    void     *ptr;
    xen_pfn_t   pfn;
    uint64_t    epoch;
} mapcache_entry_t;

#define MAPCACHE_BUCKET_SHIFT   5
#define MAPCACHE_BUCKET_SIZE    (1 << MAPCACHE_BUCKET_SHIFT)

#define MAPCACHE_BUCKET_COUNT   32

static mapcache_entry_t mapcache[MAX_DISK_IMAGES][MAPCACHE_BUCKET_SIZE *
                                 MAPCACHE_BUCKET_COUNT];
static uint64_t mapcache_epoch[MAX_DISK_IMAGES];
static int mapcache_empty[MAX_DISK_IMAGES] = {1};

volatile uint32_t mapcache_inval_cnt = 0;

static inline void *
__mapcache_lookup(int index, xen_pfn_t pfn)
{
    int     bucket;
    int     i;
    void *ptr = NULL;
    mapcache_entry_t *entry;

    bucket = pfn % MAPCACHE_BUCKET_COUNT;

    entry = &mapcache[index][(bucket * MAPCACHE_BUCKET_SIZE)];
    for (i = 0; i < MAPCACHE_BUCKET_SIZE; i++, entry++) {
        if (entry->pfn == pfn) {
            entry->epoch = mapcache_epoch[index]++;
            ptr = entry->ptr;
            break;
        }
    }

    return ptr;
}

static inline void
__mapcache_fault(int index, xen_pfn_t pfn)
{
    int         bucket;
    int         i;
    uint64_t    oldest_epoch;
    mapcache_entry_t *entry;

    /*DBG("%"PRIx64"\n", pfn);*/

    bucket = pfn % MAPCACHE_BUCKET_COUNT;

    oldest_epoch = mapcache_epoch[index];

    entry = &mapcache[index][(bucket * MAPCACHE_BUCKET_SIZE)];
    for (i = 0; i < MAPCACHE_BUCKET_SIZE; i++, entry++) {
        if (entry->epoch < oldest_epoch)
            oldest_epoch = entry->epoch;
    }

    entry = &mapcache[index][(bucket * MAPCACHE_BUCKET_SIZE)];
    for (i = 0; i < MAPCACHE_BUCKET_SIZE; i++, entry++) {
        if (entry->epoch != oldest_epoch)
            continue;

        if (entry->ptr != NULL) {
            /*DBG("unmap page %"PRIx64": %p (%d: %lu)\n", entry->pfn, entry->ptr,
                index, --count[index]);*/
            demu_unmap_guest_page(entry->ptr);
            entry->ptr = NULL;
        }

        entry->ptr = demu_map_guest_page(pfn);
        if (entry->ptr != NULL) {
            entry->pfn = pfn;
            mapcache_empty[index] = 0;
        }

        /*DBG("map page %"PRIx64": %p (%d: %lu)\n", entry->pfn, entry->ptr,
            index, ++count[index]);*/

        break;
    }
}

void *
mapcache_lookup(int index, uint64_t addr, uint64_t size)
{
    void         *ptr;
    int             faulted;

    /*assert((addr & ~TARGET_PAGE_MASK) + size <= TARGET_PAGE_SIZE);*/

    faulted = 0;
again:
    ptr = __mapcache_lookup(index, addr >> TARGET_PAGE_SHIFT);
    if (ptr == NULL) {
        if (!faulted) {
            faulted = 1;
            __mapcache_fault(index, addr >> TARGET_PAGE_SHIFT);
            goto again;
        }
        goto fail1;
    }

    return ptr + (addr & ~TARGET_PAGE_MASK);

fail1:
    DBG("fail1\n");

    return NULL;
}

void
mapcache_invalidate(int index)
{
    int i;

    if (mapcache_empty[index])
        return;

    for (i = 0; i < MAPCACHE_BUCKET_SIZE * MAPCACHE_BUCKET_COUNT; i++) {
        mapcache_entry_t *entry = &mapcache[index][i];

        if (entry->ptr != NULL) {
            /*DBG("unmap page %"PRIx64": %p (%d: %lu)\n", entry->pfn, entry->ptr,
                index, --count[index]);*/
            demu_unmap_guest_page(entry->ptr);
            entry->ptr = NULL;
            entry->pfn = 0;
            entry->epoch = 0;
        }
    }

    mapcache_epoch[index] = 0;
    mapcache_empty[index] = 1;
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

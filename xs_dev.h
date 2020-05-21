/*
 *  Helpers to read/write values via Xenbus were borrowed from Qemu:
 *  hw/xen/xen_pvdev.c
 *
 *  (c) 2008 Gerd Hoffmann <kraxel@redhat.com>
 *
 *  Copyright (C) 2020 EPAM Systems Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>
 */

#ifndef _XS_DEV_H
#define _XS_DEV_H

#include <xenctrl.h>
#include <xen/io/xenbus.h>
#include <xenstore.h>
#include <sys/types.h>

#define XEN_BUFSIZE 1024

struct xs_dev {
    struct xs_handle *xsh;
    domid_t be_domid;
    domid_t fe_domid;
    const char *type;
    int dev;
    char be[XEN_BUFSIZE];
    char path[XEN_BUFSIZE];
    char *fe;
    enum xenbus_state be_state;

    int (*connected_cb)(void *data);
    void *data;
};

struct xs_dev *xenstore_create(char *type);
void xenstore_destroy(struct xs_dev *dev);
int xenstore_connect_dom(struct xs_dev *dev, domid_t be_domid, domid_t fe_domid,
        int (*connected_cb)(void *data), void *data);
void xenstore_disconnect_dom(struct xs_dev *dev);
int xenstore_get_fd(struct xs_dev *dev);
int xenstore_poll_watches(struct xs_dev *dev);
int xenstore_get_be_domid(struct xs_dev *dev);
int xenstore_wait_fe_domid(struct xs_dev *dev);

char *xenstore_read_fe_str(struct xs_dev *dev, const char *node);
int xenstore_read_fe_int(struct xs_dev *dev, const char *node, int *ival);
char *xenstore_read_be_str(struct xs_dev *dev, const char *node);
int xenstore_read_be_int(struct xs_dev *dev, const char *node, int *ival);
int xenstore_write_be_int(struct xs_dev *dev, const char *node, int ival);

#endif /* _XS_DEV_H */


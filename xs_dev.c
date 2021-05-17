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

#include <stdarg.h>
#include <stdlib.h>
#include <pthread.h>
#include <assert.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <string.h>
#include <inttypes.h>
#include <sys/poll.h>

#include "xs_dev.h"
#include "kvm/kvm.h"

static char *xenstore_read_str(struct xs_handle *xsh,
        const char *base, const char *node)
{
    char abspath[XEN_BUFSIZE];
    unsigned int len;
    char *str, *ret = NULL;

    snprintf(abspath, sizeof(abspath), "%s/%s", base, node);
    str = xs_read(xsh, 0, abspath, &len);
    if (str != NULL) {
        ret = strdup(str);
        free(str);
    }
    return ret;
}

static int xenstore_write_str(struct xs_handle *xsh,
        const char *base, const char *node, const char *val)
{
    char abspath[XEN_BUFSIZE];

    snprintf(abspath, sizeof(abspath), "%s/%s", base, node);
    if (!xs_write(xsh, 0, abspath, val, strlen(val))) {
        return -1;
    }
    return 0;
}

static int xenstore_read_int(struct xs_handle *xsh,
        const char *base, const char *node, int *ival)
{
    char *val;
    int rc = -1;

    val = xenstore_read_str(xsh, base, node);
    if (val && 1 == sscanf(val, "%d", ival)) {
        rc = 0;
    }
    free(val);
    return rc;
}

static int xenstore_write_int(struct xs_handle *xsh,
        const char *base, const char *node, int ival)
{
    char val[12];

    snprintf(val, sizeof(val), "%d", ival);
    return xenstore_write_str(xsh, base, node, val);
}

int xenstore_read_fe_int(struct xs_dev *dev, const char *node,
                         int *ival)
{
    return xenstore_read_int(dev->xsh, dev->fe, node, ival);
}

char *xenstore_read_fe_str(struct xs_dev *dev, const char *node)
{
    return xenstore_read_str(dev->xsh, dev->fe, node);
}

char *xenstore_read_be_str(struct xs_dev *dev, const char *node)
{
    return xenstore_read_str(dev->xsh, dev->be, node);
}

int xenstore_read_be_int(struct xs_dev *dev, const char *node,
                         int *ival)
{
    return xenstore_read_int(dev->xsh, dev->be, node, ival);
}

int xenstore_write_be_int(struct xs_dev *dev, const char *node,
                          int ival)
{
    return xenstore_write_int(dev->xsh, dev->be, node, ival);
}

static int xenstore_set_be_state(struct xs_dev *dev, enum xenbus_state state)
{
    int rc;

    rc = xenstore_write_be_int(dev, "state", state);
    if (rc < 0)
        return rc;

    dev->be_state = state;
    return 0;
}

static int xenstore_wait_be_state(struct xs_dev *dev, int awaited)
{
    unsigned int num;
    int be_state;
    char **vec;

    awaited |= 1 << XenbusStateUnknown;

    while (1) {
        if (xenstore_read_be_int(dev, "state", &be_state) == -1)
            return -1;

        if ((1 << be_state) & awaited)
            return be_state;

        vec = xs_read_watch(dev->xsh, &num);
        if (!vec)
            return -1;

        free(vec);
    }
}

void xenstore_disconnect_dom(struct xs_dev *dev)
{
    if (!dev)
        return;

    xenstore_set_be_state(dev, XenbusStateClosed);

    if (dev->fe) {
        xs_unwatch(dev->xsh, dev->fe, dev->fe);
        free(dev->fe);
        dev->fe = NULL;
    }
    xs_unwatch(dev->xsh, dev->be, dev->be);
    xs_rm(dev->xsh, XBT_NULL, dev->be);

    pr_info("disconnected from dom%d\n", dev->fe_domid);

    dev->fe_domid = 0;
}

int xenstore_connect_dom(struct xs_dev *dev, domid_t be_domid, domid_t fe_domid,
        int (*connected_cb)(void *data), void *data)
{
    int state;

    if (!dev)
        return -1;

    dev->be_domid = be_domid;
    dev->fe_domid = fe_domid;
    dev->connected_cb = connected_cb;
    dev->data = data;

    snprintf(dev->be, sizeof(dev->be), "backend/%s/%d/%d",
            dev->type, dev->fe_domid, dev->devid);

    if (xenstore_read_be_int(dev, "state", &state) == -1) {
        pr_err("reading backend state failed\n");
        goto err;
    }
    dev->be_state = state;

    if (state != XenbusStateInitialising) {
        pr_err("initial backend state is wrong (%d)\n", state);
        goto err;
    }

    xenstore_set_be_state(dev, XenbusStateInitWait);

    dev->fe = xenstore_read_be_str(dev, "frontend");
    if (dev->fe == NULL) {
        pr_err("reading frontend path failed\n");
        goto err;
    }

    if (xenstore_read_fe_int(dev, "state", &state) == -1) {
        pr_err("reading frontend state failed\n");
        goto err;
    }

    if (state != XenbusStateInitialising) {
        pr_err("initial frontend state is wrong (%d)\n", state);
        goto err;
    }

    if (!xs_watch(dev->xsh, dev->be, dev->be)) {
        pr_err("watching backend path (%s) failed\n", dev->be);
        goto err;
    }

    if (!xs_watch(dev->xsh, dev->fe, dev->fe)) {
        pr_err("watching frontend path (%s) failed\n", dev->fe);
        goto err;
    }

    if (xenstore_wait_be_state(dev, 1 << XenbusStateInitWait) != XenbusStateInitWait) {
        pr_err("waiting backend state failed (%d)\n", dev->be_state);
        goto err;
    }

    if (dev->connected_cb && dev->connected_cb(dev->data) < 0)
        goto err;

    /*
     * This is not mandatory as the otherend is not xenbus-aware at all.
     * Moreover, this might lead to xl create command to fail.
     * libxl expects backend state to be XenbusStateInitWait at the domain
     * creation time. So, we cannot switch state to connected here.
     */
    /*xenstore_set_be_state(dev, XenbusStateConnected);*/

    pr_info("connected to dom%d\n", dev->fe_domid);

    return 0;

err:
    xenstore_disconnect_dom(dev);
    return -1;
}

struct xs_dev *xenstore_create(char *type)
{
    struct xs_dev *dev;

    dev = calloc(1, sizeof(struct xs_dev));
    if (!dev)
        return NULL;

    dev->xsh = xs_open(0);
    if (!dev->xsh) {
        pr_err("failed to make connection to xenstore\n");
        free(dev);
        return NULL;
    }

    dev->type = type;
    snprintf(dev->path, sizeof(dev->path), "backend/%s", dev->type);

    return dev;
}

void xenstore_destroy(struct xs_dev *dev)
{
    if (!dev)
        return;

    if (dev->xsh)
        xs_close(dev->xsh);
    free(dev);
}

int xenstore_get_be_domid(struct xs_dev *dev)
{
    int domid;
    unsigned int len;
    char *val, *end;

    if (!dev)
        return -1;

    val = xs_read(dev->xsh, XBT_NULL, "domid", &len);
    if (val) {
        domid = strtol(val, &end, 0);
        if (*end != '\0') {
            pr_err("invalid backend domid %s\n", val);
            domid = -1;
        }
        free(val);
    } else {
        /* Assume lack of node means dom0 */
        domid = 0;
    }

    return domid;
}

static bool xenstore_check_fe_exists(struct xs_dev *dev, domid_t domid)
{
    char path[XEN_BUFSIZE];
    unsigned int len;
    char *val;
    char **devid;
    unsigned int num;

    snprintf(path, sizeof(path), "backend/%s/%d",
            dev->type, domid);

    devid = xs_directory(dev->xsh, XBT_NULL, path, &num);
    if (!devid)
       return false;

    if (num > 1)
        pr_warning("got %u devices, but only single device is supported\n", num);
    dev->devid = atoi(devid[0]);
    free(devid);

    snprintf(path, sizeof(path), "/local/domain/%u/device/%s/%d",
            domid, dev->type, dev->devid);
    val = xs_read(dev->xsh, XBT_NULL, path, &len);
    if (val) {
        free(val);
        return true;
    }
    dev->devid = 0;

    return false;
}

static int xenstore_get_fe_domid(struct xs_dev *dev)
{
    static domid_t curr_domid;
    domid_t prev_domid = curr_domid;
    char **domain;
    unsigned int i, num;

    domain = xs_directory(dev->xsh, XBT_NULL, dev->path, &num);
    if (!domain)
        return -1;

    for (i = 0; i < num; i++) {
        domid_t domid = atoi(domain[i]);

        if (domid > curr_domid)
            curr_domid = domid;
    }

    free(domain);

    if (curr_domid > prev_domid && xenstore_check_fe_exists(dev, curr_domid))
        return curr_domid;

    return 0;
}

static int xenstore_poll_be_watch(struct xs_dev *dev)
{
    unsigned int num;
    char **vec;
    int rc = 0;

    vec = xs_read_watch(dev->xsh, &num);
    if (!vec)
        return -1;

    if (!strcmp(vec[XS_WATCH_PATH], dev->path))
        rc = xenstore_get_fe_domid(dev);

    free(vec);

    return rc;
}

int xenstore_wait_fe_domid(struct xs_dev *dev)
{
    struct pollfd pfd;
    int rc, domid = -1;

    if (!dev)
        return -1;

    if (!xs_watch(dev->xsh, dev->path, dev->path))
        return -1;

    pfd.fd = xs_fileno(dev->xsh);
    pfd.events = POLLIN | POLLERR | POLLHUP;
    pfd.revents = 0;

    while (domid < 0) {
        rc = poll(&pfd, 1, 1000);

        if (rc > 0 && pfd.revents & POLLIN) {
            rc = xenstore_poll_be_watch(dev);
            if (rc > 0)
                domid = rc;
        }

        if (rc < 0 && errno != EINTR)
            break;
    }

    xs_unwatch(dev->xsh, dev->path, dev->path);

    return domid;
}

static int xenstore_fe_state_changed(struct xs_dev *dev)
{
    int state;

    if (xenstore_read_fe_int(dev, "state", &state) == -1) {
        pr_err("reading frontend state failed\n");
        return -1;
    }

    switch (state) {
        case XenbusStateUnknown:
            return -1;
        case XenbusStateInitialising:
            break;
        default:
            /* We expect only two states here */
            BUG_ON(1);
            return -1;
    }

    return 0;
}

static int xenstore_be_state_changed(struct xs_dev *dev)
{
    int state;

    if (xenstore_read_be_int(dev, "state", &state) == -1) {
        pr_err("reading backend state failed\n");
        return -1;
    }

    switch (state) {
        case XenbusStateUnknown:
            return -1;
        default:
            break;
    }

    return 0;
}

int xenstore_get_fd(struct xs_dev *dev)
{
    return xs_fileno(dev->xsh);
}

int xenstore_poll_watches(struct xs_dev *dev)
{
    unsigned int num;
    char **vec;
    int rc = 0;

    if (!dev)
        return -1;

    vec = xs_read_watch(dev->xsh, &num);
    if (!vec)
        return -1;

    if (!strcmp(vec[XS_WATCH_TOKEN], dev->be))
        rc = xenstore_be_state_changed(dev);
    if (!strcmp(vec[XS_WATCH_TOKEN], dev->fe))
        rc = xenstore_fe_state_changed(dev);

    free(vec);

    return rc;
}


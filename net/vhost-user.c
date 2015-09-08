/*
 * vhost-user.c
 *
 * Copyright (c) 2013 Virtual Open Systems Sarl.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#include "clients.h"
#include "net/vhost_net.h"
#include "net/vhost-user.h"
#include "sysemu/char.h"
#include "qemu/config-file.h"
#include "qemu/error-report.h"

struct VhostUserNetPeer {
    NetClientState *nc;
    VHostNetState  *vhost_net;
};

typedef struct VhostUserState {
    CharDriverState *chr;
    bool running;
    int queues;
    struct VhostUserNetPeer peers[];
} VhostUserState;

typedef struct VhostUserChardevProps {
    bool is_socket;
    bool is_unix;
    bool is_server;
} VhostUserChardevProps;

VHostNetState *vhost_user_get_vhost_net(NetClientState *nc)
{
    VhostUserState *s = nc->opaque;
    assert(nc->info->type == NET_CLIENT_OPTIONS_KIND_VHOST_USER);
    return s->peers[nc->queue_index].vhost_net;
}

static int vhost_user_start(VhostUserState *s)
{
    VhostNetOptions options;
    VHostNetState *vhost_net;
    int max_queues;
    int i = 0;

    if (s->running)
        return 0;

    options.backend_type = VHOST_BACKEND_TYPE_USER;
    options.opaque = s->chr;

    options.net_backend = s->peers[i].nc;
    vhost_net = s->peers[i++].vhost_net = vhost_net_init(&options);

    max_queues = vhost_net_get_max_queues(vhost_net);
    if (s->queues >= max_queues) {
        error_report("you are asking more queues than supported: %d\n",
                     max_queues);
        return -1;
    }

    for (; i < s->queues; i++) {
        options.net_backend = s->peers[i].nc;

        s->peers[i].vhost_net = vhost_net_init(&options);
        if (!s->peers[i].vhost_net)
            return -1;
    }
    s->running = true;

    return 0;
}

static void vhost_user_stop(VhostUserState *s)
{
    int i;
    VHostNetState *vhost_net;

    if (!s->running)
        return;

    for (i = 0;  i < s->queues; i++) {
        vhost_net = s->peers[i].vhost_net;
        if (vhost_net)
            vhost_net_cleanup(vhost_net);
    }

    s->running = false;
}

static void vhost_user_cleanup(NetClientState *nc)
{
    VhostUserState *s = nc->opaque;
    VHostNetState *vhost_net = s->peers[nc->queue_index].vhost_net;

    if (vhost_net)
        vhost_net_cleanup(vhost_net);

    qemu_purge_queued_packets(nc);

    if (nc->queue_index == s->queues - 1)
        free(s);
}

static bool vhost_user_has_vnet_hdr(NetClientState *nc)
{
    assert(nc->info->type == NET_CLIENT_OPTIONS_KIND_VHOST_USER);

    return true;
}

static bool vhost_user_has_ufo(NetClientState *nc)
{
    assert(nc->info->type == NET_CLIENT_OPTIONS_KIND_VHOST_USER);

    return true;
}

static NetClientInfo net_vhost_user_info = {
        .type = NET_CLIENT_OPTIONS_KIND_VHOST_USER,
        .size = sizeof(NetClientState),
        .cleanup = vhost_user_cleanup,
        .has_vnet_hdr = vhost_user_has_vnet_hdr,
        .has_ufo = vhost_user_has_ufo,
};

static void net_vhost_link_down(VhostUserState *s, bool link_down)
{
    NetClientState *nc;
    int i;

    for (i = 0; i < s->queues; i++) {
        nc = s->peers[i].nc;

        nc->link_down = link_down;

        if (nc->peer) {
            nc->peer->link_down = link_down;
        }

        if (nc->info->link_status_changed) {
            nc->info->link_status_changed(nc);
        }

        if (nc->peer && nc->peer->info->link_status_changed) {
            nc->peer->info->link_status_changed(nc->peer);
        }
    }
}

static void net_vhost_user_event(void *opaque, int event)
{
    VhostUserState *s = opaque;

    switch (event) {
    case CHR_EVENT_OPENED:
        if (vhost_user_start(s) < 0)
            exit(1);
        net_vhost_link_down(s, false);
        error_report("chardev \"%s\" went up", s->chr->label);
        break;
    case CHR_EVENT_CLOSED:
        net_vhost_link_down(s, true);
        vhost_user_stop(s);
        error_report("chardev \"%s\" went down", s->chr->label);
        break;
    }
}

static int net_vhost_user_init(NetClientState *peer, const char *device,
                               const char *name, VhostUserState *s)
{
    NetClientState *nc;
    CharDriverState *chr = s->chr;
    int i;

    for (i = 0; i < s->queues; i++) {
        nc = qemu_new_net_client(&net_vhost_user_info, peer, device, name);

        snprintf(nc->info_str, sizeof(nc->info_str), "vhost-user%d to %s",
                 i, chr->label);

        /* We don't provide a receive callback */
        nc->receive_disabled = 1;

        nc->queue_index = i;
        nc->opaque      = s;

        s->peers[i].nc = nc;
    }

    qemu_chr_add_handlers(chr, NULL, NULL, net_vhost_user_event, s);

    return 0;
}

static int net_vhost_chardev_opts(void *opaque,
                                  const char *name, const char *value,
                                  Error **errp)
{
    VhostUserChardevProps *props = opaque;

    if (strcmp(name, "backend") == 0 && strcmp(value, "socket") == 0) {
        props->is_socket = true;
    } else if (strcmp(name, "path") == 0) {
        props->is_unix = true;
    } else if (strcmp(name, "server") == 0) {
        props->is_server = true;
    } else {
        error_setg(errp,
                   "vhost-user does not support a chardev with option %s=%s",
                   name, value);
        return -1;
    }
    return 0;
}

static CharDriverState *net_vhost_parse_chardev(
    const NetdevVhostUserOptions *opts, Error **errp)
{
    CharDriverState *chr = qemu_chr_find(opts->chardev);
    VhostUserChardevProps props;

    if (chr == NULL) {
        error_setg(errp, "chardev \"%s\" not found", opts->chardev);
        return NULL;
    }

    /* inspect chardev opts */
    memset(&props, 0, sizeof(props));
    if (qemu_opt_foreach(chr->opts, net_vhost_chardev_opts, &props, errp)) {
        return NULL;
    }

    if (!props.is_socket || !props.is_unix) {
        error_setg(errp, "chardev \"%s\" is not a unix socket",
                   opts->chardev);
        return NULL;
    }

    qemu_chr_fe_claim_no_fail(chr);

    return chr;
}

static int net_vhost_check_net(void *opaque, QemuOpts *opts, Error **errp)
{
    const char *name = opaque;
    const char *driver, *netdev;
    const char virtio_name[] = "virtio-net-";

    driver = qemu_opt_get(opts, "driver");
    netdev = qemu_opt_get(opts, "netdev");

    if (!driver || !netdev) {
        return 0;
    }

    if (strcmp(netdev, name) == 0 &&
        strncmp(driver, virtio_name, strlen(virtio_name)) != 0) {
        error_setg(errp, "vhost-user requires frontend driver virtio-net-*");
        return -1;
    }

    return 0;
}

int net_init_vhost_user(const NetClientOptions *opts, const char *name,
                        NetClientState *peer, Error **errp)
{
    int queues;
    const NetdevVhostUserOptions *vhost_user_opts;
    CharDriverState *chr;
    VhostUserState *s;

    assert(opts->kind == NET_CLIENT_OPTIONS_KIND_VHOST_USER);
    vhost_user_opts = opts->vhost_user;

    chr = net_vhost_parse_chardev(vhost_user_opts, errp);
    if (!chr) {
        return -1;
    }

    /* verify net frontend */
    if (qemu_opts_foreach(qemu_find_opts("device"), net_vhost_check_net,
                          (char *)name, errp)) {
        return -1;
    }

    queues = vhost_user_opts->has_queues ? vhost_user_opts->queues : 1;
    s = g_malloc0(sizeof(VhostUserState) +
                  queues * sizeof(struct VhostUserNetPeer));
    s->queues = queues;
    s->chr    = chr;

    return net_vhost_user_init(peer, "vhost_user", name, s);
}

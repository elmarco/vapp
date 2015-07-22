/*
 * vhost_server.c
 *
 * Copyright (c) 2014 Virtual Open Systems Sarl.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#include "fd_list.h"
#include "shm.h"
#include "vhost_server.h"
#include "vring.h"

typedef int (*MsgHandler)(VhostServer* vhost_server, ServerMsg* msg);

static int avail_handler_server(void* context, void* buf, size_t size);
static uintptr_t map_handler(void* context, uint64_t addr);

extern int app_running;

VhostServer* new_vhost_server(const char* path, int is_listen)
{
    VhostServer* vhost_server = (VhostServer*) calloc(1, sizeof(VhostServer));
    int idx;

    //TODO: handle errors here

    vhost_server->server = new_server(path);
    init_server(vhost_server->server, is_listen);

    vhost_server->memory.nregions = 0;

    // VringTable initalization
    vhost_server->vring_table.handler.context = (void*) vhost_server;
    vhost_server->vring_table.handler.avail_handler = avail_handler_server;
    vhost_server->vring_table.handler.map_handler = map_handler;

    for (idx = 0; idx < VHOST_CLIENT_VRING_NUM; idx++) {
        vhost_server->vring_table.vring[idx].kickfd = -1;
        vhost_server->vring_table.vring[idx].callfd = -1;
        vhost_server->vring_table.vring[idx].desc = 0;
        vhost_server->vring_table.vring[idx].avail = 0;
        vhost_server->vring_table.vring[idx].used = 0;
        vhost_server->vring_table.vring[idx].num = 0;
        vhost_server->vring_table.vring[idx].last_avail_idx = 0;
        vhost_server->vring_table.vring[idx].last_used_idx = 0;
    }

    vhost_server->buffer_size = 0;
    vhost_server->is_polling = 0;
    init_stat(&vhost_server->stat);

    vhost_server->log.eventfd = -1;

    return vhost_server;
}

int end_vhost_server(VhostServer* vhost_server)
{
    int idx;

    // End server
    end_server(vhost_server->server);
    free(vhost_server->server);
    vhost_server->server = 0;

    for (idx = 0; idx < vhost_server->memory.nregions; idx++) {
        VhostServerMemoryRegion *region = &vhost_server->memory.regions[idx];
        end_shm(vhost_server->server->path,
                (void*) (uintptr_t) region->userspace_addr,
                region->memory_size, idx);
    }

    return 0;
}

static uintptr_t _map_guest_addr(VhostServer* vhost_server, uint64_t addr)
{
    uintptr_t result = 0;
    int idx;

    for (idx = 0; idx < vhost_server->memory.nregions; idx++) {
        VhostServerMemoryRegion *region = &vhost_server->memory.regions[idx];

        if (region->guest_phys_addr <= addr
                && addr < (region->guest_phys_addr + region->memory_size)) {
            result = region->mmap_addr + addr - region->guest_phys_addr;
            break;
        }
    }

    return result;
}

static uintptr_t _map_user_addr(VhostServer* vhost_server, uint64_t addr)
{
    uintptr_t result = 0;
    int idx;

    for (idx = 0; idx < vhost_server->memory.nregions; idx++) {
        VhostServerMemoryRegion *region = &vhost_server->memory.regions[idx];

        if (region->userspace_addr <= addr
                && addr < (region->userspace_addr + region->memory_size)) {
            result = region->mmap_addr + addr - region->userspace_addr;
            break;
        }
    }

    return result;
}

static int _get_features(VhostServer* vhost_server, ServerMsg* msg)
{
    fprintf(stdout, "%s\n", __FUNCTION__);

    msg->msg.u64 = 0x1ULL << VHOST_F_LOG_ALL |
        0x1ULL << VHOST_USER_F_PROTOCOL_FEATURES;

    msg->msg.size = MEMB_SIZE(VhostUserMsg,u64);

    return 1; // should reply back
}

static int _set_features(VhostServer* vhost_server, ServerMsg* msg)
{
    fprintf(stdout, "%s\n", __FUNCTION__);
    return 0;
}

static int _set_owner(VhostServer* vhost_server, ServerMsg* msg)
{
    fprintf(stdout, "%s\n", __FUNCTION__);
    return 0;
}

static void reset_log(VhostServer* vhost_server)
{
    if (vhost_server->log.log != NULL) {
        if (munmap(vhost_server->log.log, vhost_server->log.size) != 0) {
            perror("munmap");
        }

        vhost_server->log.log = NULL;
    }
}

static int _reset_owner(VhostServer* vhost_server, ServerMsg* msg)
{
    fprintf(stdout, "%s\n", __FUNCTION__);

    reset_log(vhost_server);

    return 0;
}

static int _set_mem_table(VhostServer* vhost_server, ServerMsg* msg)
{
    int idx;
    fprintf(stdout, "%s\n", __FUNCTION__);

    vhost_server->memory.nregions = 0;

    for (idx = 0; idx < msg->msg.memory.nregions; idx++) {
        if (msg->fds[idx] > 0) {
            VhostServerMemoryRegion *region = &vhost_server->memory.regions[idx];

            region->guest_phys_addr = msg->msg.memory.regions[idx].guest_phys_addr;
            region->memory_size = msg->msg.memory.regions[idx].memory_size;
            region->userspace_addr = msg->msg.memory.regions[idx].userspace_addr;

            assert(idx < msg->fd_num);
            assert(msg->fds[idx] > 0);

            region->mmap_addr =
                    (uintptr_t) init_shm_from_fd(msg->fds[idx], region->memory_size);
            region->mmap_addr += msg->msg.memory.regions[idx].mmap_offset;

            vhost_server->memory.nregions++;
        }
    }

    fprintf(stdout, "Got memory.nregions %d\n", vhost_server->memory.nregions);

    return 0;
}

/* Get last byte of a range from offset + length.
 * Undefined for ranges that wrap around 0. */
static inline uint64_t range_get_last(uint64_t offset, uint64_t len)
{
    return offset + len - 1;
}

typedef struct VRingUsedElem
{
    uint32_t id;
    uint32_t len;
} VRingUsedElem;

typedef struct VRingUsed
{
    uint16_t flags;
    uint16_t idx;
    VRingUsedElem ring[0];
} VRingUsed;

uint64_t ring_get_used_size(Vring *ring)
{
    return offsetof(VRingUsed, ring) + sizeof(VRingUsedElem) * ring->num;
}

static uint64_t get_log_size(VhostServer* vhost_server)
{
    uint64_t log_size = 0;
    int i;

    for (i = 0; i < vhost_server->memory.nregions; ++i) {
        VhostServerMemoryRegion *reg = &vhost_server->memory.regions[i];
        uint64_t last = range_get_last(reg->guest_phys_addr,
                                       reg->memory_size);
        log_size = MAX(log_size, last / VHOST_LOG_CHUNK + 1);
    }
    for (i = 0; i < VHOST_CLIENT_VRING_NUM; ++i) {
        Vring *ring = &vhost_server->vring_table.vring[i];
        uint64_t last =
            range_get_last(ring->log_guest_addr,
                           ring_get_used_size(ring));
        log_size = MAX(log_size, last / VHOST_LOG_CHUNK + 1);
    }
    return log_size;
}

static int _set_log_base(VhostServer* vhost_server, ServerMsg* msg)
{
    int fd = -1;
    uint64_t size;

    fprintf(stdout, "%s\n", __FUNCTION__);

    assert(msg->fd_num <= 1);

    reset_log(vhost_server);

    if (msg->fd_num == 1) {
        fd = msg->fds[0];
        size = get_log_size(vhost_server) * sizeof(*(vhost_server->log.log));
        vhost_server->log.log =
            init_shm_from_fd(fd, size);
        vhost_server->log.size = size;
        fprintf(stdout, "fd:%d size:%lu ptr:%p\n",
                fd, size, vhost_server->log.log);
    }

    if (fd != -1)
        close(fd);

    return 0;
}

static int _set_log_fd(VhostServer* vhost_server, ServerMsg* msg)
{
    assert(msg->fd_num == 1);

    if (vhost_server->log.eventfd != -1)
        close(vhost_server->log.eventfd);

    vhost_server->log.eventfd = msg->fds[0];

    fprintf(stdout, "Got log eventfd 0x%x\n", vhost_server->log.eventfd);

    return 0;
}

static int _set_vring_num(VhostServer* vhost_server, ServerMsg* msg)
{
    fprintf(stdout, "%s\n", __FUNCTION__);

    int idx = msg->msg.state.index;

    assert(idx<VHOST_CLIENT_VRING_NUM);

    vhost_server->vring_table.vring[idx].num = msg->msg.state.num;

    return 0;
}

static int _set_vring_addr(VhostServer* vhost_server, ServerMsg* msg)
{
    fprintf(stdout, "%s\n", __FUNCTION__);

    int idx = msg->msg.addr.index;

    assert(idx<VHOST_CLIENT_VRING_NUM);

    vhost_server->vring_table.vring[idx].desc =
            (struct vring_desc*) _map_user_addr(vhost_server,
                    msg->msg.addr.desc_user_addr);
    vhost_server->vring_table.vring[idx].avail =
            (struct vring_avail*) _map_user_addr(vhost_server,
                    msg->msg.addr.avail_user_addr);
    vhost_server->vring_table.vring[idx].used =
            (struct vring_used*) _map_user_addr(vhost_server,
                    msg->msg.addr.used_user_addr);
    vhost_server->vring_table.vring[idx].flags =
            msg->msg.addr.flags;

    vhost_server->vring_table.vring[idx].last_used_idx =
            vhost_server->vring_table.vring[idx].used->idx;

    vhost_server->vring_table.vring[idx].log_guest_addr =
        msg->msg.addr.log_guest_addr;

    return 0;
}

static int _set_vring_base(VhostServer* vhost_server, ServerMsg* msg)
{
    fprintf(stdout, "%s\n", __FUNCTION__);

    int idx = msg->msg.state.index;

    assert(idx<VHOST_CLIENT_VRING_NUM);

    vhost_server->vring_table.vring[idx].last_avail_idx = msg->msg.state.num;

    return 0;
}

static int _get_vring_base(VhostServer* vhost_server, ServerMsg* msg)
{
    fprintf(stdout, "%s\n", __FUNCTION__);

    int idx = msg->msg.state.index;

    assert(idx<VHOST_CLIENT_VRING_NUM);

    msg->msg.state.num = vhost_server->vring_table.vring[idx].last_avail_idx;
    msg->msg.size = MEMB_SIZE(VhostUserMsg,state);

    return 1; // should reply back
}

static int avail_handler_server(void* context, void* buf, size_t size)
{
    VhostServer* vhost_server = (VhostServer*) context;
    size_t hdr_len = sizeof(struct virtio_net_hdr);

    if (size + hdr_len > BUFFER_SIZE)
        return -1;

    // copy the packet to our private buffer
    memcpy(vhost_server->buffer + hdr_len, buf, size);
    vhost_server->buffer_size = size + hdr_len;

#ifdef DUMP_PACKETS
    dump_buffer(buf, size);
#endif

    return 0;
}

static uintptr_t map_handler(void* context, uint64_t addr)
{
    VhostServer* vhost_server = (VhostServer*) context;
    return _map_guest_addr(vhost_server, addr);
}

static int _poll_avail_vring(VhostServer* vhost_server, int idx)
{
    uint32_t count = 0;

    // if vring is already set, process the vring
    if (vhost_server->vring_table.vring[idx].desc) {
        count = process_avail_vring(&vhost_server->vring_table, idx,
                                    &vhost_server->log);
#ifndef DUMP_PACKETS

        update_stat(&vhost_server->stat, count);
        print_stat(&vhost_server->stat);
#endif
    }

    return count;
}

static int _kick_server(FdNode* node)
{
    VhostServer* vhost_server = (VhostServer*) node->context;
    int kickfd = node->fd;
    ssize_t r;
    uint64_t kick_it = 0;

    r = read(kickfd, &kick_it, sizeof(kick_it));

    if (r < 0) {
        perror("recv kick");
    } else if (r == 0) {
        fprintf(stdout, "Kick fd closed\n");
        del_fd_list(&vhost_server->server->fd_list, FD_READ, kickfd);
    } else {
#if 0
        fprintf(stdout, "Got kick %"PRId64"\n", kick_it);
#endif
        _poll_avail_vring(vhost_server, VHOST_CLIENT_VRING_IDX_TX);
    }

    return 0;
}

static int _set_vring_kick(VhostServer* vhost_server, ServerMsg* msg)
{
    fprintf(stdout, "%s\n", __FUNCTION__);

    int idx = msg->msg.u64 & VHOST_USER_VRING_IDX_MASK;
    int validfd = (msg->msg.u64 & VHOST_USER_VRING_NOFD_MASK) == 0;

    assert(idx<VHOST_CLIENT_VRING_NUM);
    if (validfd) {
        assert(msg->fd_num == 1);

        vhost_server->vring_table.vring[idx].kickfd = msg->fds[0];

        fprintf(stdout, "Got kickfd 0x%x\n", vhost_server->vring_table.vring[idx].kickfd);

        if (idx == VHOST_CLIENT_VRING_IDX_TX) {
            add_fd_list(&vhost_server->server->fd_list, FD_READ,
                    vhost_server->vring_table.vring[idx].kickfd,
                    (void*) vhost_server, _kick_server);
            fprintf(stdout, "Listening for kicks on 0x%x\n", vhost_server->vring_table.vring[idx].kickfd);
        }
        vhost_server->is_polling = 0;
    } else {
        fprintf(stdout, "Got empty kickfd. Start polling.\n");
        vhost_server->is_polling = 1;
    }

    return 0;
}

static int _set_vring_call(VhostServer* vhost_server, ServerMsg* msg)
{
    fprintf(stdout, "%s\n", __FUNCTION__);

    int idx = msg->msg.u64 & VHOST_USER_VRING_IDX_MASK;
    int validfd = (msg->msg.u64 & VHOST_USER_VRING_NOFD_MASK) == 0;

    assert(idx<VHOST_CLIENT_VRING_NUM);
    if (validfd) {
        assert(msg->fd_num == 1);

        vhost_server->vring_table.vring[idx].callfd = msg->fds[0];

        fprintf(stdout, "Got callfd 0x%x\n", vhost_server->vring_table.vring[idx].callfd);
    }

    return 0;
}

static int _set_vring_err(VhostServer* vhost_server, ServerMsg* msg)
{
    fprintf(stdout, "%s\n", __FUNCTION__);
    return 0;
}

static int _get_proto_features(VhostServer* vhost_server, ServerMsg* msg)
{
    fprintf(stdout, "%s\n", __FUNCTION__);

    msg->msg.u64 = 0x1ULL << VHOST_USER_PROTOCOL_F_LOG_SHMFD;
    msg->msg.size = MEMB_SIZE(VhostUserMsg,u64);

    return 1; // should reply back
}

static int _set_proto_features(VhostServer* vhost_server, ServerMsg* msg)
{
    fprintf(stdout, "%s\n", __FUNCTION__);
    return 0;
}

static MsgHandler msg_handlers[VHOST_USER_MAX] = {
        0,                  // VHOST_USER_NONE
        _get_features,      // VHOST_USER_GET_FEATURES
        _set_features,      // VHOST_USER_SET_FEATURES
        _set_owner,         // VHOST_USER_SET_OWNER
        _reset_owner,       // VHOST_USER_RESET_OWNER
        _set_mem_table,     // VHOST_USER_SET_MEM_TABLE
        _set_log_base,      // VHOST_USER_SET_LOG_BASE
        _set_log_fd,        // VHOST_USER_SET_LOG_FD
        _set_vring_num,     // VHOST_USER_SET_VRING_NUM
        _set_vring_addr,    // VHOST_USER_SET_VRING_ADDR
        _set_vring_base,    // VHOST_USER_SET_VRING_BASE
        _get_vring_base,    // VHOST_USER_GET_VRING_BASE
        _set_vring_kick,    // VHOST_USER_SET_VRING_KICK
        _set_vring_call,    // VHOST_USER_SET_VRING_CALL
        _set_vring_err,     // VHOST_USER_SET_VRING_ERR
        _get_proto_features,// VHOST_USER_GET_PROTOCOL_FEATURES
        _set_proto_features,// VHOST_USER_SET_PROTOCOL_FEATURES
        };

static int in_msg_server(void* context, ServerMsg* msg)
{
    VhostServer* vhost_server = (VhostServer*) context;
    int result = 0;

    fprintf(stdout, "Processing message: %s\n", cmd_from_vhostmsg(&msg->msg));

    assert(msg->msg.request > VHOST_USER_NONE && msg->msg.request < VHOST_USER_MAX);

    if (msg_handlers[msg->msg.request]) {
        result = msg_handlers[msg->msg.request](vhost_server, msg);
    }

    return result;
}

static int poll_server(void* context)
{
    VhostServer* vhost_server = (VhostServer*) context;
    int tx_idx = VHOST_CLIENT_VRING_IDX_TX;
    int rx_idx = VHOST_CLIENT_VRING_IDX_RX;

    if (vhost_server->vring_table.vring[rx_idx].desc) {
        // process TX ring
        if (vhost_server->is_polling) {
            _poll_avail_vring(vhost_server, tx_idx);
        }

        // process RX ring
        if (vhost_server->buffer_size) {
            // send a packet from the buffer
            reply_vring(&vhost_server->vring_table, rx_idx,
                      vhost_server->buffer, vhost_server->buffer_size,
                      &vhost_server->log);

            // signal the client
            call(&vhost_server->vring_table, rx_idx);

            // mark the buffer empty
            vhost_server->buffer_size = 0;
        }
    }

    return 0;
}

static AppHandlers vhost_server_handlers =
{
        .context = 0,
        .in_handler = in_msg_server,
        .poll_handler = poll_server
};

int run_vhost_server(VhostServer* vhost_server)
{
    vhost_server_handlers.context = vhost_server;
    set_handler_server(vhost_server->server, &vhost_server_handlers);

    start_stat(&vhost_server->stat);

    app_running = 1; // externally modified
    while (app_running) {
        loop_server(vhost_server->server);
    }

    stop_stat(&vhost_server->stat);

    return 0;
}

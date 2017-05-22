// Copyright (C) 2016 iNuron NV
//
// This file is part of Open vStorage Open Source Edition (OSE),
// as available from
//
//      http://www.openvstorage.org and
//      http://www.openvstorage.com.
//
// This file is free software; you can redistribute it and/or modify it
// under the terms of the GNU Affero General Public License v3 (GNU AGPLv3)
// as published by the Free Software Foundation, in version 3 as it comes in
// the LICENSE.txt file of the Open vStorage OSE distribution.
// Open vStorage is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY of any kind.

#ifndef __NETWORK_XIO_CLIENT_H_
#define __NETWORK_XIO_CLIENT_H_

#include "../NetworkXioProtocol.h"
#include "NetworkHAContext.h"
#include "common_priv.h"
#include "internal.h"

#include <boost/thread/lock_guard.hpp>
#include <youtils/SpinLock.h>
#include <youtils/Logger.h>

#include <libxio.h>
#include <msgpack.hpp>

#include <queue>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <memory>

namespace libovsvolumedriver
{

MAKE_EXCEPTION(XioClientCreateException, fungi::IOException);
MAKE_EXCEPTION(XioClientRegHandlerException, fungi::IOException);

class NetworkXioClient
{
public:
    NetworkXioClient(const std::string& uri,
                     const uint64_t qd,
                     NetworkHAContext& ha_ctx);

    ~NetworkXioClient();

    struct session_data
    {
        xio_context *ctx;
        xio_session *session;
        bool disconnected = false;
        bool disconnecting = false;
        bool connection_error = false;
        bool session_established = false;
    };

    struct xio_msg_s
    {
        xio_msg xreq;
        void *opaque = nullptr;
        NetworkXioMsg msg;
        std::string s_msg;
        void *priv[2] = {nullptr, nullptr};

        void
        set_opaque(ovs_aio_request *request)
        {
            opaque = static_cast<void*>(request);
        }

        ovs_aio_request*
        get_request()
        {
            return reinterpret_cast<ovs_aio_request*>(opaque);
        }

        uint64_t
        get_request_id()
        {
            return reinterpret_cast<ovs_aio_request*>(opaque)->_id;
        }

        RequestOp
        get_request_op()
        {
            return reinterpret_cast<ovs_aio_request*>(opaque)->_op;
        }
    };

    void
    xio_send_open_request(const std::string& volname,
                          ovs_aio_request *request);

    void
    xio_send_close_request(ovs_aio_request *request);

    void
    xio_send_read_request(void *buf,
                          const uint64_t size_in_bytes,
                          const uint64_t offset_in_bytes,
                          ovs_aio_request *request);

    void
    xio_send_write_request(const void *buf,
                           const uint64_t size_in_bytes,
                           const uint64_t offset_in_bytes,
                           ovs_aio_request *request);

    void
    xio_send_flush_request(ovs_aio_request *request);

    void
    xio_get_volume_uri(const char* volume_name,
                       std::string& volume_uri,
                       ovs_aio_request *request);

    void
    xio_list_cluster_node_uri(std::vector<std::string>& uris,
                              ovs_aio_request *request);

    void
    xio_get_cluster_multiplier(const char *volume_name,
                               uint32_t *cluster_multiplier,
                               ovs_aio_request *request);

    void
    xio_get_page(const char *volume_name,
                 const ClusterAddress ca,
                 ClusterLocationPage& cl,
                 ovs_aio_request *request);

    void
    xio_get_clone_namespace_map(const char *volume_name,
                                CloneNamespaceMap& cn,
                                ovs_aio_request *request);

    void
    xio_create_volume(const char* volume_name,
                      size_t size,
                      ovs_aio_request *request);

    void
    xio_remove_volume(const char* volume_name,
                      ovs_aio_request *request);

    void
    xio_truncate_volume(const char* volume_name,
                        uint64_t offset,
                        ovs_aio_request *request);

    void
    xio_stat_volume(const std::string& volume_name,
                    uint64_t *size,
                    ovs_aio_request *request);

    void
    xio_list_volumes(std::vector<std::string>& volumes,
                     ovs_aio_request *request);

    void
    xio_list_snapshots(const char* volume_name,
                       std::vector<std::string>& snapshots,
                       uint64_t *size,
                       ovs_aio_request *request);

    void
    xio_create_snapshot(const char* volume_name,
                        const char* snap_name,
                        int64_t timeout,
                        ovs_aio_request *request);

    void
    xio_delete_snapshot(const char* volume_name,
                        const char* snap_name,
                        ovs_aio_request *request);

    void
    xio_rollback_snapshot(const char* volume_name,
                          const char* snap_name,
                          ovs_aio_request *request);

    void
    xio_is_snapshot_synced(const char* volume_name,
                           const char* snap_name,
                           ovs_aio_request *request);

    int
    on_session_event(xio_session *session,
                     xio_session_event_data *event_data);

    int
    on_response(xio_session *session,
                xio_msg* reply,
                int last_in_rxq);

    int
    on_msg_error(xio_session *session,
                 xio_status error,
                 xio_msg_direction direction,
                 xio_msg *msg);

    void
    evfd_stop_loop(int fd, int events, void *data);

    void
    run(std::promise<bool>& promise);

    bool
    is_queue_empty();

    xio_msg_s*
    pop_request();

    void
    push_request(xio_msg_s *req);

    void
    xstop_loop();

    bool
    is_dtl_in_sync()
    {
        return dtl_in_sync_;
    }

    static void
    xio_destroy_ctx_shutdown(xio_context *ctx);
private:
    std::shared_ptr<xio_context> ctx;
    std::shared_ptr<xio_session> session;
    xio_connection *conn;
    xio_session_params params;
    xio_connection_params cparams;

    std::string uri_;
    bool stopping;
    bool stopped;
    std::thread xio_thread_;

    mutable fungi::SpinLock inflight_lock;
    std::queue<xio_msg_s*> inflight_reqs;

    xio_session_ops ses_ops;
    bool disconnected;
    bool disconnecting;
    bool connect_error;

    int64_t nr_req_queue;

    volumedriverfs::EventFD evfd;

    NetworkHAContext& ha_ctx_;
    std::atomic<bool> dtl_in_sync_;

    void
    xio_run_loop_worker();

    void
    shutdown();

    void
    handle_ctrl_response(const NetworkXioMsg& imsg,
                         xio_msg_s *msg,
                         xio_msg *reply);

    void
    handle_get_volume_uri(xio_msg_s *msg,
                          xio_iovec_ex *sglist);

    void
    handle_list_cluster_node_uri(xio_msg_s *xmsg,
                                 xio_iovec_ex *sglist,
                                 int vec_size);

    void
    handle_get_cluster_multiplier(xio_msg_s *xmsg,
                                  uint64_t cluster_size);

    void
    handle_get_page_vector(xio_msg_s *xmsg,
                           xio_iovec_ex *sglist);

    void
    handle_get_clone_namespace_map(xio_msg_s *xmsg,
                                   xio_iovec_ex *sglist);

    void
    handle_stat(xio_msg_s *xio_msg,
                uint64_t volsize);

    void
    xio_msg_prepare(xio_msg_s *xmsg);

    void
    handle_list_volumes(xio_msg_s *xmsg,
                        xio_iovec_ex *sglist,
                        int vec_size);

    void
    handle_list_snapshots(xio_msg_s *xmsg,
                          xio_iovec_ex *sglist,
                          int vec_size,
                          size_t size);

    void
    create_vec_from_buf(xio_msg_s *xmsg,
                        xio_iovec_ex *sglist,
                        int vec_size);

    msgpack::object_handle
    msgpack_obj_handle(xio_iovec_ex *sglist);

    void
    set_dtl_in_sync(const NetworkXioMsgOpcode op,
                    const ssize_t retval,
                    const int errval,
                    const bool dtl_in_sync)
    {
        switch (op)
        {
        case NetworkXioMsgOpcode::WriteRsp:
        case NetworkXioMsgOpcode::FlushRsp:
            if (retval >= 0 && errval == 0)
            {
                dtl_in_sync_ = dtl_in_sync;
            }
            break;
        default:
            break;
        }
    }

    bool
    is_ha_enabled() const
    {
        return ha_ctx_.is_ha_enabled();
    }

    bool
    maybe_fail_request(xio_msg_s *req)
    {
        RequestOp op = req->get_request_op();
        assert(op != RequestOp::Noop);
        if (is_op_to_fail(op))
        {
            return true;
        }
        else
        {
            return is_ha_enabled() ? false : true;
        }
    }

    bool
    is_op_to_fail(RequestOp op)
    {
        switch (op)
        {
        case RequestOp::Open:
        case RequestOp::Close:
        case RequestOp::GetVolumeUri:
        case RequestOp::ListClusterNodeUri:
        case RequestOp::GetClusterMultiplier:
        case RequestOp::GetPage:
        case RequestOp::GetCloneNamespaceMap:
            return true;
        default:
            return false;
        }
    }

    bool
    is_ctrl_op_to_handle(NetworkXioMsgOpcode op)
    {
        switch (op)
        {
        case NetworkXioMsgOpcode::ReadRsp:
        case NetworkXioMsgOpcode::WriteRsp:
        case NetworkXioMsgOpcode::FlushRsp:
            return false;
        default:
            return true;
        }
    }

    void
    insert_seen_request(xio_msg_s *xio_msg)
    {
        if (is_ha_enabled())
        {
            uint64_t id = xio_msg->get_request_id();
            RequestOp op = xio_msg->get_request_op();
            assert(op != RequestOp::Noop);
            switch (op)
            {
            case RequestOp::Read:
            case RequestOp::Write:
            case RequestOp::Flush:
            case RequestOp::AsyncFlush:
                ha_ctx_.insert_seen_request(id);
                break;
            default:
                break;
            }
        }
    }
};

typedef std::shared_ptr<NetworkXioClient> NetworkXioClientPtr;

} //namespace libovsvolumedriver

#endif //__NETWORK_XIO_CLIENT_H_

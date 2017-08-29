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

#ifndef __NETWORK_XIO_SERVER_H_
#define __NETWORK_XIO_SERVER_H_

#include "FileSystem.h"
#include "NetworkXioIOHandler.h"
#include "NetworkXioRequest.h"
#include "NetworkXioWorkQueue.h"
#include "NetworkXioMempool.h"

#include <map>
#include <tuple>
#include <memory>
#include <libxio.h>

#include <youtils/Uri.h>

namespace volumedriverfs
{

MAKE_EXCEPTION(FailedBindXioServer, fungi::IOException);
MAKE_EXCEPTION(FailedCreateXioContext, fungi::IOException);
MAKE_EXCEPTION(FailedCreateMempool, fungi::IOException);
MAKE_EXCEPTION(FailedCreateEventfd, fungi::IOException);
MAKE_EXCEPTION(FailedRegisterEventHandler, fungi::IOException);

class NetworkXioServer
{
public:
    NetworkXioServer(FileSystem&,
                     const youtils::Uri&,
                     size_t snd_rcv_queue_depth,
                     unsigned int workqueue_max_threads,
                     unsigned int workqueue_ctrl_max_threads,
                     const std::atomic<uint32_t>& max_neighbour_distance,
                     const NetworkXioSlabConfigs& cfg);

    ~NetworkXioServer();

    NetworkXioServer(const NetworkXioServer&) = delete;

    NetworkXioServer&
    operator=(const NetworkXioServer&) = delete;

    int
    on_request(xio_session *session,
               xio_msg *req,
               int last_in_rxq,
               void *cb_user_context);

    int
    on_session_event(xio_session *session,
                     xio_session_event_data *event_data);

    int
    on_new_session(xio_session *session,
                   xio_new_session_req *req);

    int
    on_msg_send_complete(xio_session *session,
                         xio_msg *msg,
                         void *cb_user_context);

    int
    on_msg_error(xio_session *session,
                 xio_status error,
                 xio_msg_direction direction,
                 xio_msg *msg);

    void
    run(std::promise<void> promise);

    void
    shutdown();

    void
    xio_send_reply(NetworkXioRequest *req);

    void
    evfd_stop_loop(int fd, int events, void *data);
private:
    DECLARE_LOGGER("NetworkXioServer");

    FileSystem& fs_;
    youtils::Uri uri_;
    bool stopping;

    std::mutex mutex_;
    std::condition_variable cv_;
    bool stopped;
    EventFD evfd;
    size_t queue_depth;
    unsigned int wq_max_threads;
    unsigned int wq_ctrl_max_threads;
    const std::atomic<uint32_t>& max_neighbour_distance;
    const NetworkXioSlabConfigs& xio_mpool_cfg;

    NetworkXioWorkQueuePtr wq_;
    NetworkXioWorkQueuePtr wq_ctrl_;

    mutable fungi::SpinLock finished_lock;
    boost::intrusive::list<NetworkXioRequest> finished_list;

    std::shared_ptr<xio_context> ctx;
    std::shared_ptr<xio_server> server;
    NetworkXioMempoolPtr mpool;

    int
    create_session_connection(xio_session *session,
                              xio_session_event_data *event_data);

    void
    destroy_session_connection(xio_session *session,
                               xio_session_event_data *event_data);

    NetworkXioRequest*
    allocate_request(NetworkXioClientData *cd, xio_msg *xio_req);

    void
    deallocate_request(NetworkXioRequest *req);

    void
    free_request(NetworkXioRequest *req);

    NetworkXioClientData*
    allocate_client_data();

    void
    prepare_msg_reply(NetworkXioRequest *req);
};

} //namespace

#endif //__NETWORK_XIO_SERVER_H_

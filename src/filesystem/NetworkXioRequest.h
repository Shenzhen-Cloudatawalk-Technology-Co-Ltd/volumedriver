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

#ifndef __NETWORK_XIO_REQUEST_H_
#define __NETWORK_XIO_REQUEST_H_

#include "ClientInfo.h"

#include "NetworkXioWork.h"
#include "NetworkXioCommon.h"
#include "NetworkXioMempool.h"
#include "NetworkXioRequestFwd.h"

#include <boost/intrusive/list.hpp>
#include <boost/intrusive_ptr.hpp>

namespace volumedriverfs
{

struct NetworkXioClientData;

struct NetworkXioRequest
    : public boost::intrusive::list_base_hook<>
{
private:
    NetworkXioRequest(NetworkXioClientData *cdata,
                      xio_msg *xreq)
        : refcnt(0)
        , op(NetworkXioMsgOpcode::Noop)
        , data(nullptr)
        , data_len(0)
        , size(0)
        , dtl_in_sync(true)
        , offset(0)
        , retval(0)
        , errval(0)
        , opaque(0)
        , xio_req(xreq)
        , mem_block(nullptr)
        , from_pool(true)
        , cd(cdata)
    {
        memset(&xio_reply, 0, sizeof(xio_reply));
    }

    ~NetworkXioRequest() = default;

    std::atomic<uint64_t> refcnt;

public:
    NetworkXioMsgOpcode op;

    void *data;
    unsigned int data_len;
    size_t size;
    bool dtl_in_sync;
    union
    {
        uint64_t offset;
        uint64_t u64;
        int64_t i64;
    };

    ssize_t retval;
    int errval;
    uintptr_t opaque;

    Work work;

    xio_msg *xio_req;
    xio_msg xio_reply;
    xio_reg_mem reg_mem;
    slab_mem_block *mem_block;
    bool from_pool;

    NetworkXioClientData *cd;

    std::string s_msg;
    boost::future<void> future;

    static NetworkXioRequestPtr
    create(NetworkXioClientData *cdata,
           xio_msg *xreq)
    {
        NetworkXioRequestPtr req(new NetworkXioRequest(cdata,
                                                      xreq));
        ASSERT(req->refcnt == 1);
        return req;
    }

    friend inline void
    intrusive_ptr_add_ref(NetworkXioRequest* req)
    {
        ASSERT(req);
        ++req->refcnt;
    }

    friend inline void
    intrusive_ptr_release(NetworkXioRequest* req)
    {
        if (req and --req->refcnt == 0)
        {
            delete req;
        }
    }
};

class NetworkXioServer;
class NetworkXioIOHandler;

struct NetworkXioClientData
{
    xio_session *session;
    xio_connection *conn;
    NetworkXioMempoolPtr mpool;
    std::atomic<bool> disconnected;
    std::atomic<uint64_t> refcnt;
    NetworkXioServer *server;
    NetworkXioIOHandler *ioh;
    ClientInfoTag tag;
};

} //namespace

#endif //__NETWORK_XIO_REQUEST_H_

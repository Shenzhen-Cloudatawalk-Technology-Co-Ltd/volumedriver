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

#ifndef FAILOVERCACHEPROTOCOL_H
#define FAILOVERCACHEPROTOCOL_H

#include "../ClusterLocation.h"
#include "../FailOverCacheCommand.h"
#include "../OwnerTag.h"

#include "fungilib/Protocol.h"
#include "fungilib/Socket.h"
#include "fungilib/SocketServer.h"
#include "fungilib/Thread.h"
#include "fungilib/IOBaseStream.h"

namespace volumedriver
{

class FailOverCacheEntry;

namespace failovercache
{

class CapnProtoDispatcher;
class FailOverCacheAcceptor;
class Backend;

class FailOverCacheProtocol
    : public fungi::Protocol
{
public:
    FailOverCacheProtocol(std::unique_ptr<fungi::Socket>,
                          fungi::SocketServer&,
                          FailOverCacheAcceptor&,
                          const boost::chrono::microseconds busy_loop_duration);

    ~FailOverCacheProtocol();

    virtual void
    start() override final;

    virtual void
    run() override final;

    void
    stop();

    virtual const char*
    getName() const override final
    {
        return "FailOverCacheProtocol";
    };

private:
    DECLARE_LOGGER("FailOverCacheProtocol");

    friend class CapnProtoDispatcher;

    std::shared_ptr<Backend> cache_;
    std::unique_ptr<fungi::Socket> sock_;
    fungi::IOBaseStream stream_;
    OwnerTag owner_tag_;
    fungi::Thread* thread_;
    FailOverCacheAcceptor& fact_;
    bool use_rs_;
    std::atomic<bool> stop_;
    int pipes_[2];
    boost::chrono::microseconds busy_loop_duration_;

    std::unique_ptr<CapnProtoDispatcher> capnp_dispatcher_;

    void
    do_add_entries_(std::vector<FailOverCacheEntry>,
                    std::unique_ptr<uint8_t[]>);

    void
    addEntries_();

    void
    getEntries_();

    void
    do_flush_();

    void
    Flush_();

    void
    register_();

    // TODO: use backend::Namespace instead of std::string
    // (this will be a viral change in this corner of the code base)
    void
    do_register_(const std::string& nspace,
                 const ClusterSize,
                 const OwnerTag);

    void
    unregister_();

    void
    do_unregister_();

    void
    getSCO_();

    std::pair<ClusterLocation, ClusterLocation>
    do_get_range_();

    void
    getSCORange_();

    void
    do_clear_();

    void
    Clear_();

    void
    returnOk();

    void
    returnNotOk();

    void
    do_remove_up_to_(SCO);

    void
    removeUpTo_();

    void
    processFailOverCacheEntry_(volumedriver::ClusterLocation cli,
                               int64_t lba,
                               const byte* buf,
                               int64_t size,
                               bool cork);

    bool
    poll_(volumedriver::FailOverCacheCommand& cmd);

    void
    capnp_request_();

    void
    raw_cork_(bool);

    template<typename... Args>
    void
    handle_fungi_(void (FailOverCacheProtocol::*)(Args...),
                  Args...);
};

}

}

#endif // FAILOVERCACHEPROTOCOL_H

// Local Variables: **
// mode: c++ **
// End: **

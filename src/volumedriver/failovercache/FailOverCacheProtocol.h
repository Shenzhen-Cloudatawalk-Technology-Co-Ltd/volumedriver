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

#include "fungilib/Protocol.h"
#include "fungilib/Socket.h"
#include "fungilib/SocketServer.h"
#include "fungilib/Thread.h"
#include "fungilib/IOBaseStream.h"

namespace volumedriver
{

namespace failovercache
{

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

    std::shared_ptr<Backend> cache_;
    std::unique_ptr<fungi::Socket> sock_;
    fungi::IOBaseStream stream_;
    fungi::Thread* thread_;
    FailOverCacheAcceptor& fact_;
    bool use_rs_;
    std::atomic<bool> stop_;
    int pipes_[2];
    boost::chrono::microseconds busy_loop_duration_;

    void
    addEntries_();

    void
    getEntries_();

    void
    Flush_();

    void
    register_();

    void
    unregister_();

    void
    getSCO_();

    void
    getSCORange_();

    void
    Clear_();

    void
    returnOk();

    void
    returnNotOk();

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
};

}

}

#endif // FAILOVERCACHEPROTOCOL_H

// Local Variables: **
// mode: c++ **
// End: **

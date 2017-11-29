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

#ifndef FAILOVERCACHEACCEPTOR_H
#define FAILOVERCACHEACCEPTOR_H

#include "FailOverCacheProtocol.h"
#include "BackendFactory.h"

#include "../FailOverCacheStreamers.h"

#include <unordered_map>

#include <boost/optional.hpp>
#include <boost/thread/lock_guard.hpp>
#include <boost/thread/mutex.hpp>

#include "fungilib/ProtocolFactory.h"

namespace volumedrivertest
{

class FailOverCacheTestContext;

}

namespace failovercache
{

class FailOverCacheAcceptor
    : public fungi::ProtocolFactory
{
    friend class volumedrivertest::FailOverCacheTestContext;

public:
    FailOverCacheAcceptor(const boost::optional<boost::filesystem::path>& root,
                          const boost::optional<size_t> file_backend_buffer_size,
                          const boost::chrono::microseconds busy_loop_duration);

    virtual ~FailOverCacheAcceptor();

    virtual fungi::Protocol*
    createProtocol(std::unique_ptr<fungi::Socket>,
                   fungi::SocketServer& parentServer) override final;

    virtual const char *
    getName() const override final
    {
        return "FailOverCacheAcceptor";
    }

    void
    remove(Backend&);

    using BackendPtr = std::shared_ptr<Backend>;

    BackendPtr
    lookup(const volumedriver::CommandData<volumedriver::FailOverCacheCommand::Register>&);

    void
    removeProtocol(FailOverCacheProtocol* prot)
    {
        boost::lock_guard<decltype(mutex_)> g(mutex_);
        protocols.remove(prot);
    }

private:
    DECLARE_LOGGER("FailOverCacheAcceptor");

    // protects the map / protocols.
    boost::mutex mutex_;

    using Map = std::unordered_map<std::string, BackendPtr>;
    Map map_;

    std::list<FailOverCacheProtocol*> protocols;
    BackendFactory factory_;
    const boost::chrono::microseconds busy_loop_duration_;

    // for use by testers
    BackendPtr
    find_backend_(const std::string&);
};

}
#endif // FAILOVERCACHEACCEPTOR_H

// Local Variables: **
// mode : c++ **
// End: **

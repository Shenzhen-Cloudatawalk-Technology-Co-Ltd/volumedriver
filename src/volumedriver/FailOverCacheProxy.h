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

#ifndef FAILOVERCACHEPROXY_H
#define FAILOVERCACHEPROXY_H

#include "FailOverCacheCommand.h"
#include "SCOProcessorInterface.h"
#include "SCO.h"

#include "failovercache/fungilib/Socket.h"
#include "failovercache/fungilib/IOBaseStream.h"
#include "failovercache/fungilib/Buffer.h"

#include <vector>

#include <boost/ptr_container/ptr_vector.hpp>
#include <boost/scoped_array.hpp>
#include <boost/thread/future.hpp>

namespace volumedriver
{

class FailOverCacheConfig;
class FailOverCacheEntry;
class Volume;

class FailOverCacheProxy
{
public:
    FailOverCacheProxy(const FailOverCacheConfig&,
                       const Namespace&,
                       const LBASize,
                       const ClusterMultiplier,
                       const boost::chrono::milliseconds request_timeout,
                       const boost::optional<boost::chrono::milliseconds>& connect_timeout);

    FailOverCacheProxy(const FailOverCacheProxy&) = delete;

    FailOverCacheProxy&
    operator=(const FailOverCacheProxy&) = delete;

    ~FailOverCacheProxy();

    boost::future<void>
    addEntries(std::vector<FailOverCacheEntry>);

    boost::future<void>
    addEntries(const std::vector<ClusterLocation>&,
               uint64_t addr,
               const uint8_t*);

    // returns the SCO size - 0 indicates a problem.
    // Z42: throw instead!
    uint64_t
    getSCOFromFailOver(SCO,
                       SCOProcessorFun);

    void
    getSCORange(SCO& oldest,
                SCO& youngest);

    void
    clear();

    boost::future<void>
    flush();

    void
    removeUpTo(const SCO) throw ();

    void
    setRequestTimeout(const boost::chrono::milliseconds);

    void
    setBusyLoopDuration(const boost::chrono::microseconds);

    void
    getEntries(SCOProcessorFun);

    void
    delete_failover_dir()
    {
        LOG_INFO("Setting delete_failover_dir_ to true");
        delete_failover_dir_ = true;
    }

    LBASize
    lba_size() const
    {
        return lba_size_;
    }

    ClusterMultiplier
    cluster_multiplier() const
    {
        return cluster_mult_;
    }

private:
    DECLARE_LOGGER("FailOverCacheProxy");

    std::unique_ptr<fungi::Socket> socket_;
    fungi::IOBaseStream stream_;
    const Namespace ns_;
    const LBASize lba_size_;
    const ClusterMultiplier cluster_mult_;
    bool delete_failover_dir_;

    void
    register_();

    void
    unregister_();

    void
    check_(const char* desc);

    uint64_t
    getObject_(SCOProcessorFun,
               bool cork_per_cluster);

    template<FailOverCacheCommand,
             typename... Args>
    void
    send_(Args&&...);

    template<FailOverCacheCommand>
    void
    send_();
};

}

#endif // FAILOVERCACHEPROXY_H

// Local Variables: **
// mode: c++ **
// End: **

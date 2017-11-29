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

#ifndef VD_FAILOVER_CACHE_ASYNCBRIDGE_H
#define VD_FAILOVER_CACHE_ASYNCBRIDGE_H

#include "FailOverCacheBridgeCommon.h"
#include "FailOverCacheStreamers.h"
#include "FailOverCacheProxy.h"
#include "FailOverCacheClientInterface.h"
#include "SCO.h"

#include "failovercache/fungilib/Runnable.h"
#include "failovercache/fungilib/Thread.h"

#include <boost/thread/condition_variable.hpp>
#include <boost/thread/mutex.hpp>

#include <youtils/FileDescriptor.h>
#include <youtils/IOException.h>

namespace volumedriver
{

class FailOverCacheTester;

class FailOverCacheAsyncBridge
    : public fungi::Runnable
    , public FailOverCacheClientInterface
{
    friend class FailOverCacheTester;

public:
    FailOverCacheAsyncBridge(const LBASize,
                             const ClusterMultiplier,
                             const size_t max_entries,
                             const std::atomic<unsigned>& write_trigger);

    FailOverCacheAsyncBridge(const FailOverCacheAsyncBridge&) = delete;

    ~FailOverCacheAsyncBridge() = default;

    FailOverCacheAsyncBridge&
    operator=(const FailOverCacheAsyncBridge&) = delete;

    virtual void
    initialize(DegradedFun) override;

    virtual void
    run() override;

    virtual void
    destroy(SyncFailOverToBackend) override;

    virtual boost::future<void>
    addEntries(const std::vector<ClusterLocation>&,
               uint64_t start_address,
               const uint8_t* data) override;

    virtual bool
    backup() override;

    virtual void
    newCache(std::unique_ptr<FailOverCacheProxy> cache) override;

    virtual void
    setRequestTimeout(const boost::chrono::seconds) override;

    virtual void
    setBusyLoopDuration(const boost::chrono::microseconds) override;

    virtual void
    removeUpTo(const SCO& sconame) override;

    virtual uint64_t
    getSCOFromFailOver(SCO sconame,
                       SCOProcessorFun processor) override;

    virtual boost::future<void>
    Flush() override;

    virtual void
    Clear() override;

    virtual FailOverCacheMode
    mode() const override;

    // fungi::Runnable
    virtual const char*
    getName() const override;

private:
    DECLARE_LOGGER("FailOverCacheAsyncBridge");

    void
    addEntry(ClusterLocation loc,
             uint64_t lba,
             const uint8_t* buf,
             size_t bufsize);

    void
    setThrottling(bool v);

    void
    maybe_swap_(bool new_sco);

    void
    init_cache_();

    void
    flush_();

    ClusterSize
    cluster_size_() const
    {
        return ClusterSize(cluster_multiplier_ * lba_size_);
    }

    std::unique_ptr<FailOverCacheProxy> cache_;

    // mutex_: protects stop_ and cache_
    // new_ones_mutex_: protects newOnes / oldOnes + buffers
    // lock order: mutex_ before new_ones_mutex_
    boost::mutex mutex_;
    boost::mutex new_ones_mutex_;
    boost::condition_variable condvar_;

    std::vector<FailOverCacheEntry> newOnes;
    std::vector<FailOverCacheEntry> oldOnes;
    const LBASize lba_size_;
    const ClusterMultiplier cluster_multiplier_;
    std::vector<uint8_t> newData;
    std::vector<uint8_t> oldData;
    const std::atomic<unsigned>& write_trigger_;

    // make configurable?
    static const boost::chrono::seconds timeout_;
    // number of entries before scheduling a write
    fungi::Thread* thread_;
    bool stop_;
    bool throttling;

    DegradedFun degraded_fun_;
};

}

#endif // VD_FAILOVER_CACHE_ASYNCBRIDGE

// Local Variables: **
// mode: c++ **
// End: **

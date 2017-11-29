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

#ifndef VD_FAILOVER_CACHE_SYNCBRIDGE_H
#define VD_FAILOVER_CACHE_SYNCBRIDGE_H

#include "FailOverCacheBridgeCommon.h"
#include "FailOverCacheStreamers.h"
#include "FailOverCacheProxy.h"
#include "FailOverCacheClientInterface.h"
#include "SCO.h"

#include <youtils/FileDescriptor.h>
#include <youtils/IOException.h>

namespace volumedriver
{

class FailOverCacheTester;

class FailOverCacheSyncBridge
    : public FailOverCacheClientInterface
{
    friend class FailOverCacheTester;

public:
    explicit FailOverCacheSyncBridge(const size_t max_entries);

    FailOverCacheSyncBridge(const FailOverCacheSyncBridge&) = delete;

    FailOverCacheSyncBridge&
    operator=(const FailOverCacheSyncBridge&) = delete;

    ~FailOverCacheSyncBridge() = default;

    virtual void
    initialize(DegradedFun) override;

    virtual void
    destroy(SyncFailOverToBackend) override;

    virtual boost::future<void>
    addEntries(const std::vector<ClusterLocation>&,
               uint64_t start_address,
               const uint8_t* data) override;

    virtual bool
    backup() override;

    virtual void
    newCache(std::unique_ptr<FailOverCacheProxy>) override;

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

private:
    DECLARE_LOGGER("FailOverCacheSyncBridge");

    boost::mutex mutex_;
    std::unique_ptr<FailOverCacheProxy> cache_;
    DegradedFun degraded_fun_;

    void
    handleException(std::exception& e,
                    const char* where);
};

}

#endif // VD_FAILOVER_CACHE_SYNCBRIDGE

// Local Variables: **
// mode: c++ **
// End: **

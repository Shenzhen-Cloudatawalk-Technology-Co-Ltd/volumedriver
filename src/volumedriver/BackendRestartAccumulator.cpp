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

#include "BackendRestartAccumulator.h"
#include "NSIDMap.h"
#include "VolumeFactory.h"

#include <youtils/Assert.h>

namespace volumedriver
{

namespace be = backend;
namespace yt = youtils;

BackendRestartAccumulator::BackendRestartAccumulator(NSIDMap& nsid,
                                                     const boost::optional<youtils::UUID>& start_cork,
                                                     const boost::optional<youtils::UUID>& end_cork)
    : nsid_(nsid)
    , start_cork_(start_cork)
    , end_cork_(end_cork)
    , start_seen_(start_cork_ == boost::none ? true : false)
    , end_seen_(false)
{
    LOG_INFO("cork range: (" << start_cork_ << ", " << end_cork_ << "]");

    VERIFY(nsid_.empty());

    // TODO: Why is this here?
    VERIFY(not (start_cork != boost::none and
                start_cork == end_cork));
}

void
BackendRestartAccumulator::operator()(const SnapshotPersistor& sp,
                                      be::BackendInterfacePtr& bi,
                                      const SnapshotName& snapshot_name,
                                      SCOCloneID clone_id)
{
    nsid_.set(clone_id,
              bi->clone());

    OrderedTLogIds tlogs;

    auto walk_tlogs([&](const OrderedTLogIds& tlog_ids)
                    {
                        VERIFY(not end_seen_);

                        for (const auto& tlog_id : tlog_ids)
                        {
                            if (start_cork_ != boost::none and
                                TLogId(*start_cork_) == tlog_id)
                            {
                                VERIFY(not start_seen_);
                                start_seen_ = true;
                            }
                            else if (start_seen_)
                            {
                                tlogs.push_back(tlog_id);
                            }

                            if (end_cork_ != boost::none and
                                TLogId(*end_cork_) == tlog_id)
                            {
                                end_seen_ = true;
                                break;
                            }
                        }
                    });

    if (not end_seen_)
    {
        for (const auto& snap : sp.getSnapshots())
        {
            walk_tlogs(snap.getOrderedTLogIds());

            // check the cork IDs of empty snapshots:
            if (not start_seen_ and
                start_cork_ != boost::none and
                *start_cork_ == snap.getCork())
            {
                start_seen_ = true;
            }

            if (not end_seen_ and
                end_cork_ != boost::none and
                *end_cork_ == snap.getCork())
            {
                end_seen_ = true;
            }

            if (end_seen_)
            {
                break;
            }
        }
    }

    if(clone_id == SCOCloneID(0))
    {
        if (not end_seen_)
        {
            walk_tlogs(sp.getCurrentTLogsWrittenToBackend());
        }
    }
    else
    {
        VERIFY(not snapshot_name.empty());
    }

    TODO("AR: don't bother adding empty log vectors?");

    clone_tlogs_.emplace_back(std::make_pair(clone_id,
                                             std::move(tlogs)));
}

const CloneTLogs&
BackendRestartAccumulator::clone_tlogs() const
{
    if (not start_seen_)
    {
        VERIFY(start_cork_ != boost::none);

        LOG_ERROR("start cork " << start_cork_ << " not seen");
        throw CorkNotFoundException("start cork not seen",
                                    start_cork_->str().c_str());
    }

    if (end_cork_ != boost::none and not end_seen_)
    {
        LOG_ERROR("end cork " << end_cork_ << " not seen");
        throw CorkNotFoundException("end cork not seen",
                                    end_cork_->str().c_str());
    }

#ifndef NDEBUG
    for (const auto& p : clone_tlogs_)
    {
        LOG_INFO("Clone TLogs for " << (int)p.first);

        for (const auto& t : p.second)
        {
            LOG_INFO("\t" << t);
        }
    }
#endif

    return clone_tlogs_;
}

}

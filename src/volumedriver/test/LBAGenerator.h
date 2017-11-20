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

#ifndef LBAGENERATOR_H_
#define LBAGENERATOR_H_

#include <youtils/SourceOfUncertainty.h>
#include <youtils/Md5.h>

#include "../ClusterLocationAndHash.h"
#include "../CombinedTLogReader.h"
#include "../TLogReaderInterface.h"
#include "../VolumeConfig.h"

namespace volumedrivertest
{

using namespace volumedriver;

class LBAGenerator
    : public volumedriver::TLogReaderInterface
{
    DECLARE_LOGGER("LBAGenerator");

public:
    LBAGenerator(float random,
                 uint64_t total_size, // how many bytes to write to the volume
                 uint64_t range, //not inclusive
                 uint64_t offset = 0)
        : random_(random)
        , clusters_to_write_(total_size / VolumeConfig::default_cluster_size())
        , clusters_written_(0)
        , range_(range / VolumeConfig::default_cluster_size())
        , offset_(offset / VolumeConfig::default_cluster_size())
        , entry_(offset_ + range_ - 1,
                 volumedriver::ClusterLocationAndHash(volumedriver::ClusterLocation(1),
                                                      youtils::Weed()))
    {
        LOG_TRACE("random " << random_ << ", clusters to write " << clusters_to_write_ <<
                  "range " << range_ << ", offset " << offset_);
    }

    virtual const volumedriver::Entry*
    nextAny()
    {
        if(clusters_written_ >= clusters_to_write_)
        {
            return nullptr;
        }

        ++clusters_written_;

        const float x = rand_((uint64_t)0, range_); // use a uniform_real_distribution instead?
        ClusterAddress ca;
        if (x < random_ * range_)
        {
            ca = rand_(offset_, range_);
            LOG_TRACE("random, ca " << ca);
        }
        else
        {
            ca = (entry_.clusterAddress() - offset_ + 1) % range_ + offset_;
            LOG_TRACE("sequential, ca " << ca);
        }

        entry_ = volumedriver::Entry(ca,
                                     entry_.clusterLocationAndHash());

        return &entry_;
    }

private:
    const float random_;
    uint64_t clusters_to_write_;
    uint64_t clusters_written_;
    const uint64_t range_;
    const uint64_t offset_;
    volumedriver::Entry entry_;
    youtils::SourceOfUncertainty rand_;
};

class LBAGenGen
{
public:
    explicit LBAGenGen(uint64_t clusters,
                       uint64_t vsize = 1ULL << 40,
                       float random = 0.0,
                       uint64_t offset = 0)
        : clusters_(clusters)
        , vsize_(vsize)
        , random_(random)
        , offset_(offset)
    {}

    volumedriver::TLogGenItem
    operator()()
    {
        volumedriver::TLogGenItem t(new LBAGenerator(random_,
                                                     clusters_ * 4096,
                                                     vsize_,
                                                     offset_));
        return t;
    }

private:
    const uint64_t clusters_;
    const uint64_t vsize_;
    const float random_;
    const uint64_t offset_;
};

}

#endif /* LBAGENERATOR_H_ */

// Local Variables: **
// mode: c++ **
// End: **

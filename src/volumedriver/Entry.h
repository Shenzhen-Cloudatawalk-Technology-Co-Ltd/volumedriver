// Copyright 2015 iNuron NV
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef _ENTRY_H_
#define _ENTRY_H_

#include "ClusterLocationAndHash.h"
#include "Types.h"

#include <iosfwd>
#include <vector>

#include <youtils/IOException.h>
#include <youtils/Logging.h>
#include <youtils/CheckSum.h>

namespace volumedriver
{

MAKE_EXCEPTION(InvalidEntryException, fungi::IOException);

class Entry
{
public:
    enum class Type
        : uint64_t
    {
        SyncTC = 0,
        TLogCRC = 1,
        SCOCRC = 2,
        LOC = 3
    };

    // SyncTC
    Entry();

    // LOC
    Entry(const ClusterAddress&,
          const ClusterLocationAndHash&);

    // SCO or TLog CRC
    Entry(const CheckSum& cs,
          Type t);

    ~Entry() = default;

    Entry(const Entry&) = default;

    Entry&
    operator=(const Entry&) = default;

#define MAKE_CHECKER(checker, etype)            \
    inline bool                                 \
    checker() const                             \
    {                                           \
        try                                     \
        {                                       \
            return type() == etype;             \
        }                                       \
        catch (InvalidEntryException&)          \
        {                                       \
            return false;                       \
        }                                       \
    }

MAKE_CHECKER(isLocation, Type::LOC)
MAKE_CHECKER(isTLogCRC, Type::TLogCRC)
MAKE_CHECKER(isSCOCRC, Type::SCOCRC)
MAKE_CHECKER(isSync, Type::SyncTC)

#undef MAKE_CHECKER

    CheckSum::value_type
    getCheckSum() const
    {
        return clusteraddress_ bitand checksum_mask_;
    }

    static inline uint32_t
    getDataSize()
    {
        return sizeof(Entry);
    }

    ClusterAddress
    clusterAddress() const;

    ClusterLocation
    clusterLocation() const;

    const ClusterLocationAndHash&
    clusterLocationAndHash() const;

    friend std::ostream& operator<<(std::ostream& outp,
                                    const Entry & e);

    Type
    type() const;

    // OVS-685 / SSOBF-10034:
    // We artifically limit the volume size to 4G clusters (== 16TiB w/ 4k clusters)
    // as a sanity check. Bigger volumes are not supported by the codebase at the moment
    // anyway as that would lead to inordinate resource consumption e.g. during restart.
    static constexpr ClusterAddress max_valid_cluster_address_ = (1ULL << 32) - 1;

    static ClusterAddress
    max_valid_cluster_address()
    {
        return max_valid_cluster_address_;
    }

private:
    DECLARE_LOGGER("Entry");

    ClusterAddress clusteraddress_;
    ClusterLocationAndHash loc_and_hash_;

    static constexpr uint64_t checksum_shift_ = 32;
    static constexpr uint64_t checksum_mask_ = (1ULL << checksum_shift_) - 1;
};

static_assert(sizeof(Entry) == sizeof(ClusterAddress) + sizeof(ClusterLocationAndHash),
              "Entry size assumption does not hold");

bool operator==(const Entry& , const Entry& );

inline std::ostream&
operator<<(std::ostream& os,
           const Entry::Type& t)
{
    switch(t)
    {
    case Entry::Type::SyncTC:
        return os << "SyncTC";
    case Entry::Type::TLogCRC:
        return os << "TLogCRC";
    case Entry::Type::SCOCRC:
        return os << "SCOCRC";
    case Entry::Type::LOC:
        return os << "LOC";
    }
    UNREACHABLE
}

}

#endif /* _ENTRY_H_ */

// Local Variables: **
// mode: c++ **
// End: **

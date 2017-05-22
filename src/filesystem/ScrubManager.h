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

#ifndef VFS_SCRUB_MANAGER_H_
#define VFS_SCRUB_MANAGER_H_

#include "ClusterId.h"
#include "Object.h"

#include <boost/thread/mutex.hpp>

#include <youtils/IOException.h>
#include <youtils/LockedArakoon.h>
#include <youtils/Logging.h>
#include <youtils/PeriodicAction.h>
#include <youtils/Serialization.h>

#include <backend/Garbage.h>

#include <volumedriver/ScrubbingCleanup.h>
#include <volumedriver/ScrubReply.h>

namespace youtils
{
class UUID;
}

namespace volumedriver
{
class SnapshotName;
}

namespace scrubbing
{
class ScrubReply;
}

namespace volumedriverfstest
{

class ScrubManagerTest;
class ScrubbingTest;

}

namespace volumedriverfs
{

class ObjectRegistry;

// Ideas:
// * ScrubReplies are first put on a parent_scrubs_ queue (actually a map)
//   (which is persisted to Arakoon)
// * a PeriodicAction
//   ** periodically walks over this queue and applies the scrub results to the
//      destination volume
//   ** determines on on success all children and creates CloneScrubDescriptors
//      puts them to a clone_scrubs_ queue (which is again persisted to Arakoon) and
//      removes the entry from the parent_scrubs_ queue
//   ** walks over the clone_scrubs_ queue and applies the scrub results to the
//      respective clone (removing them on success, once the CloneScrubDescriptors
//      data structure is empty garbage can be collected).
class ScrubManager
{
    friend class volumedriverfstest::ScrubbingTest;
    friend class volumedriverfstest::ScrubManagerTest;

public:
    MAKE_EXCEPTION(Exception,
                   fungi::IOException);
    MAKE_EXCEPTION(NoSuchObjectException,
                   Exception);
    MAKE_EXCEPTION(NotAVolumeException,
                   Exception);

    struct Clone;
    using ClonePtr = boost::shared_ptr<Clone>;
    using ClonePtrList = std::list<ClonePtr>;

    struct Clone
    {
        ObjectId id;
        ClonePtrList clones;

        explicit Clone(const ObjectId& i)
            : id(i)
        {}

        Clone() = default;

        ~Clone() = default;

        template<typename A>
        void
        serialize(A& ar,
                  const unsigned /* version */)
        {
            ar & BOOST_SERIALIZATION_NVP(id);
            ar & BOOST_SERIALIZATION_NVP(clones);
        }
    };

    using MaybeGarbage = boost::optional<backend::Garbage>;
    using ParentScrubs = std::map<scrubbing::ScrubReply,
                                  ObjectId>;

    using ApplyScrubReplyFun =
        std::function<MaybeGarbage(const ObjectId&,
                                   const scrubbing::ScrubReply&,
                                   const volumedriver::ScrubbingCleanup)>;

    using CollectGarbageFun = std::function<void(backend::Garbage)>;

    using BuildScrubTreeFun =
        std::function<ClonePtrList(const ObjectId&,
                                   const volumedriver::SnapshotName&)>;

    ScrubManager(ObjectRegistry&,
                 std::shared_ptr<youtils::LockedArakoon>,
                 const bool enabled = false);

    ScrubManager(ObjectRegistry&,
                 std::shared_ptr<youtils::LockedArakoon>,
                 const std::atomic<uint64_t>& period_secs,
                 const bool enabled,
                 ApplyScrubReplyFun,
                 BuildScrubTreeFun,
                 CollectGarbageFun);

    ~ScrubManager() = default;

    ScrubManager(const ScrubManager&) = delete;

    ScrubManager&
    operator=(const ScrubManager&) = delete;

    void
    queue_scrub_reply(const ObjectId&,
                      const scrubbing::ScrubReply&);

    void
    destroy();

    ParentScrubs
    get_parent_scrubs();

    std::vector<scrubbing::ScrubReply>
    get_clone_scrubs();

    ClonePtrList
    get_scrub_tree(const scrubbing::ScrubReply&);

    struct Counters
    {
        uint64_t parent_scrubs_ok = 0;
        uint64_t parent_scrubs_nok = 0;
        uint64_t clone_scrubs_ok = 0;
        uint64_t clone_scrubs_nok = 0;

        bool
        operator==(const Counters& other) const
        {
#define EQ(x)                                    \
            x == other.x

            return
                EQ(parent_scrubs_ok) and
                EQ(parent_scrubs_nok) and
                EQ(clone_scrubs_ok) and
                EQ(clone_scrubs_nok)
                ;
#undef EQ
        }

        bool
        operator!=(const Counters& other) const
        {
            return not operator==(other);
        }

        template<typename Archive>
        void
        serialize(Archive& ar,
                  const unsigned /*version*/)
        {
            ar & BOOST_SERIALIZATION_NVP(parent_scrubs_ok);
            ar & BOOST_SERIALIZATION_NVP(parent_scrubs_nok);
            ar & BOOST_SERIALIZATION_NVP(clone_scrubs_ok);
            ar & BOOST_SERIALIZATION_NVP(clone_scrubs_nok);
        }

        static constexpr const char* serialization_name = "ScrubManagerCounters";
    };

    Counters
    get_counters() const;

    bool
    enabled() const
    {
        return enabled_;
    }

    void
    enable(bool e)
    {
        enabled_ = e;
    }

private:
    DECLARE_LOGGER("ScrubManager");

    ObjectRegistry& registry_;
    const std::string parent_scrubs_key_;
    const std::string clone_scrubs_index_key_;

    std::shared_ptr<youtils::LockedArakoon> larakoon_;

    ApplyScrubReplyFun apply_scrub_reply_;
    BuildScrubTreeFun build_scrub_tree_;
    CollectGarbageFun collect_garbage_;
    std::atomic<bool> enabled_;
    std::unique_ptr<youtils::PeriodicAction> periodic_action_;

    mutable boost::mutex counters_lock_;
    Counters counters_;

    void
    work_();

    std::string
    make_clone_scrub_key_(const youtils::UUID&) const;

    std::string
    make_scrub_garbage_key_(const youtils::UUID&) const;

    void
    apply_to_parent_(const ObjectId&,
                     const scrubbing::ScrubReply&);

    void
    queue_to_clones_(const ObjectId&,
                     const scrubbing::ScrubReply&,
                     backend::Garbage);

    void
    drop_parent_(const ObjectId&,
                 const scrubbing::ScrubReply&);

    void
    apply_to_clones_(const youtils::UUID&,
                     const scrubbing::ScrubReply&);

    void
    apply_to_clone_(const youtils::UUID&,
                    const ObjectId&,
                    const scrubbing::ScrubReply&);

    boost::optional<bool>
    apply_(const ObjectId&,
           const scrubbing::ScrubReply&,
           const volumedriver::ScrubbingCleanup,
           MaybeGarbage&);

    void
    drop_clone_(const youtils::UUID&,
                const ObjectId&,
                const scrubbing::ScrubReply&,
                const bool keep_children);

    void
    finalize_(const youtils::UUID&,
              const scrubbing::ScrubReply&);

    void
    collect_scrub_garbage_();

    using PerNodeGarbage = std::list<youtils::UUID>;

    PerNodeGarbage
    per_node_garbage_();
};

std::ostream&
operator<<(std::ostream&,
           const ScrubManager::Counters&);

}

BOOST_CLASS_VERSION(volumedriverfs::ScrubManager::Counters, 1);

#endif // !VFS_SCRUB_MANAGER_H_

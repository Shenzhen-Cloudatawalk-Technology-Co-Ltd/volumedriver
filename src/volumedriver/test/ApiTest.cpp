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

#include <boost/thread.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/thread/condition_variable.hpp>
#include <boost/foreach.hpp>

#include "VolManagerTestSetup.h"

#include "../Api.h"
#include "../SnapshotPersistor.h"
#include "../VolumeConfigParameters.h"

#include <future>

#include <fstream>

#include <youtils/Assert.h>

namespace volumedriver
{

namespace be = backend;

#define LOCK_MGMT()                                             \
    fungi::ScopedLock __l(api::getManagementMutex())

class ApiTest
    : public VolManagerTestSetup
{
public:
    ApiTest()
        : VolManagerTestSetup("ApiTest")
    {
        // dontCatch(); uncomment if you want an unhandled exception to cause a crash, e.g. to get a stacktrace
    }

    // simulates a *looong* volume restart.
    void
    fakeRestart(const Namespace& ns,
                boost::mutex* mutex,
                boost::condition_variable* cond,
                bool* run)
    {
        {
            boost::unique_lock<boost::mutex> l(*mutex);
            const VolumeConfig cfg(VanillaVolumeConfigParameters(VolumeId(ns.str()),
                                                                 ns,
                                                                 VolumeSize(1ULL << 20),
                                                                 new_owner_tag()));
            setNamespaceRestarting(ns, cfg);
            *run = true;
        }

        cond->notify_one();

        boost::unique_lock<boost::mutex> l(*mutex);


        while (*run)
        {
            cond->wait(l);
        }

        clearNamespaceRestarting(ns);
    }

    void
    writeAndCheckFirstCluster(WeakVolumePtr vol,
                              byte pattern)
    {
        {
            const std::vector<byte> wbuf(4096, pattern);
            LOCK_MGMT();
            api::Write(vol, Lba(0), &wbuf[0], wbuf.size());
        }

        {
            LOCK_MGMT();
            api::Sync(vol);
        }

        checkFirstCluster(vol, pattern);
    }

    void
    checkFirstCluster(WeakVolumePtr vol,
                      byte pattern)
    {
        const std::vector<byte> ref(4096, pattern);
        std::vector<byte> rbuf(ref.size());

        {
            LOCK_MGMT();
            api::Read(vol, Lba(0), &rbuf[0], rbuf.size());
        }

        EXPECT_TRUE(ref == rbuf);
    }

    void
    testApi(//const Namespace& ns,
            // const Namespace& clone_ns,
            SnapshotName& snap)
    {
        auto ns1ptr = make_random_namespace();
        auto ns2ptr = make_random_namespace();

        const Namespace& ns = ns1ptr->ns();
        const Namespace& clone_ns = ns2ptr->ns();


        const VolumeId vol_id(ns.str());
        const VolumeSize vol_size(1 << 30);

        const OwnerTag parent_owner_tag(new_owner_tag());

        {
            LOCK_MGMT();
            ASSERT_NO_THROW(api::createNewVolume(VanillaVolumeConfigParameters(vol_id,
                                                                               ns,
                                                                               vol_size,
                                                                               parent_owner_tag)));
        }

        WeakVolumePtr vol;

        {
            LOCK_MGMT();
            ASSERT_NO_THROW(vol = api::getVolumePointer(vol_id));
        }

        writeAndCheckFirstCluster(vol, '1');

        {
            LOCK_MGMT();
            EXPECT_NO_THROW(snap = api::createSnapshot(vol_id,
                                                       SnapshotMetaData(),
                                                       &snap));
        }

        bool sync(false);

        do
        {
            sleep (1);
            LOCK_MGMT();
            EXPECT_NO_THROW(sync = api::isVolumeSynced(vol_id));
        }
        while (not sync);

        std::list<VolumeId> vol_list;
        {
            LOCK_MGMT();
            EXPECT_NO_THROW(api::getVolumeList(vol_list));
        }

        bool found = false;
        BOOST_FOREACH(VolumeId& i, vol_list)
        {
            if (i == vol_id)
            {
                found = true;
            }
        }

        EXPECT_TRUE(found);

        std::list<SnapshotName> snap_list;
        {
            LOCK_MGMT();
            EXPECT_NO_THROW(api::showSnapshots(vol_id, snap_list));
        }

        EXPECT_EQ((unsigned)1, snap_list.size());
        EXPECT_EQ(snap, snap_list.front());

        {
            LOCK_MGMT();
            EXPECT_NO_THROW(api::destroyVolume(vol_id,
                                               DeleteLocalData::F,
                                               RemoveVolumeCompletely::F,
                                               DeleteVolumeNamespace::F,
                                               ForceVolumeDeletion::F));
        }

        const VolumeId clone_id(clone_ns.str());
        const OwnerTag clone_owner_tag(new_owner_tag());

        {
            LOCK_MGMT();

            const auto params = CloneVolumeConfigParameters(clone_id,
                                                            clone_ns,
                                                            ns,
                                                            clone_owner_tag)
                .parent_snapshot(snap);

            ASSERT_NO_THROW(api::createClone(params,
                                             PrefetchVolumeData::T));
        }

        WeakVolumePtr clone;

        {
            LOCK_MGMT();
            EXPECT_NO_THROW(clone = api::getVolumePointer(clone_id));
        }

        vol_list.clear();

        {
            LOCK_MGMT();
            EXPECT_NO_THROW(api::getVolumeList(vol_list));
        }

        found = false;
        BOOST_FOREACH(VolumeId& i, vol_list)
        {
            if (i == clone_id)
            {
                found = true;
            }
        }

        EXPECT_TRUE(found);

        writeAndCheckFirstCluster(clone, '2');

        SnapshotName snap2;
        {
            LOCK_MGMT();
            EXPECT_NO_THROW(snap2 = api::createSnapshot(clone_id));
        }

        writeAndCheckFirstCluster(clone, '3');

        {
            LOCK_MGMT();
            EXPECT_NO_THROW(api::scheduleBackendSync(clone_id));
        }

        sync = false;
        do
        {
            sleep (1);
            LOCK_MGMT();
            EXPECT_NO_THROW(sync = api::isVolumeSynced(clone_id));
        }
        while (not sync);

        {
            LOCK_MGMT();
            EXPECT_NO_THROW(api::destroyVolume(clone_id,
                                               DeleteLocalData::T,
                                               RemoveVolumeCompletely::F,
                                               DeleteVolumeNamespace::F,
                                               ForceVolumeDeletion::F));
        }

        {
            LOCK_MGMT();
            ASSERT_NO_THROW(api::backend_restart(clone_ns,
                                                 clone_owner_tag,
                                                 PrefetchVolumeData::F,
                                                 IgnoreFOCIfUnreachable::T));
        }

        {
            LOCK_MGMT();
            ASSERT_NO_THROW(clone = api::getVolumePointer(clone_id));
        }

        checkFirstCluster(clone, '3');

        {
            LOCK_MGMT();
            EXPECT_NO_THROW(api::restoreSnapshot(clone_id, snap2));
        }

        checkFirstCluster(clone, '2');

        {
            LOCK_MGMT();
            EXPECT_NO_THROW(api::destroyVolume(clone_id,
                                               DeleteLocalData::T,
                                               RemoveVolumeCompletely::F,
                                               DeleteVolumeNamespace::F,
                                               ForceVolumeDeletion::F));
        }

        {
            LOCK_MGMT();
            ASSERT_NO_THROW(api::local_restart(ns,
                                               parent_owner_tag,
                                               FallBackToBackendRestart::F,
                                               IgnoreFOCIfUnreachable::T));
        }

        {
            LOCK_MGMT();
            ASSERT_NO_THROW(vol = api::getVolumePointer(vol_id));
        }

        checkFirstCluster(vol, '1');

        {
            LOCK_MGMT();
            EXPECT_NO_THROW(api::destroyVolume(vol_id,
                                               DeleteLocalData::F,
                                               RemoveVolumeCompletely::F,
                                               DeleteVolumeNamespace::F,
                                               ForceVolumeDeletion::F));
        }
    }

protected:
    DECLARE_LOGGER("ApiTest");
};

TEST_P(ApiTest, QueueCount)
{
    auto nspaceptr = make_random_namespace();
    const Namespace& nspace = nspaceptr->ns();

    SharedVolumePtr v = newVolume("volume1",
                                  nspace);

    {
        SCOPED_BLOCK_BACKEND(*v);
        for(int i = 0; i < 5; i++)
        {
            writeToVolume(*v,
                          Lba(0),
                          4096,
                          "xyz");
        }

        PerformanceCounters c;

        {
            fungi::ScopedLock l(api::getManagementMutex());
            EXPECT_EQ(0U,
                      api::getQueueCount(VolumeId("volume1")));
            EXPECT_EQ(0U,
                      api::getQueueSize(VolumeId("volume1")));
            EXPECT_EQ(0U,
                      api::performance_counters(VolumeId("volume1")).backend_write_request_size.sum());
        }

        v->createSnapshot(SnapshotName("snap1"));
        {
            fungi::ScopedLock l(api::getManagementMutex());
            EXPECT_EQ(1U,
                      api::getQueueCount(VolumeId("volume1")));
            EXPECT_EQ(v->get_config().getSCOSize(),
                      api::getQueueSize(VolumeId("volume1")));
            EXPECT_EQ(0U,
                      api::performance_counters(VolumeId("volume1")).backend_write_request_size.sum());
        }
    }
    while(not v->isSyncedToBackend())
    {
        sleep(1);
    }
    {
        fungi::ScopedLock l(api::getManagementMutex());
        EXPECT_EQ(0U,
                  api::getQueueCount(VolumeId("volume1")));
        EXPECT_EQ(0U,
                  api::getQueueSize(VolumeId("volume1")));
        EXPECT_EQ(20480U,
                  api::performance_counters(VolumeId("volume1")).backend_write_request_size.sum());
        EXPECT_EQ(20480U,
                  api::getStored(VolumeId("volume1")));
    }
};

TEST_P(ApiTest, SCOCacheMountPoints)
{
    const std::string mp1(directory_.string() + "/mp1");
    const std::string mp2(directory_.string() + "/mp2");
    const std::string mp3(directory_.string() + "/mp3");

    fs::remove(mp1);
    fs::remove(mp2);
    fs::remove(mp3);

    const std::string dummy(directory_.string() + "dummy");

    uint64_t fs_size = FileUtils::filesystem_size(directory_);

    EXPECT_THROW(api::addSCOCacheMountPoint(mp1,
                                            std::numeric_limits<uint64_t>::max()),
                 std::exception) <<
        "scocache mountpoint directory must exist";

    fs::create_directories(mp1);
    fs::create_directories(mp2);
    fs::create_directories(mp3);

    EXPECT_THROW(api::addSCOCacheMountPoint(mp1, fs_size * 2),
                 fungi::IOException) <<
        "specified size must not exceed filesystem size";

    EXPECT_NO_THROW(api::addSCOCacheMountPoint(mp1,
                                               std::numeric_limits<uint64_t>::max()));
    EXPECT_NO_THROW(api::addSCOCacheMountPoint(mp2,
                                               std::numeric_limits<uint64_t>::max()));
    EXPECT_NO_THROW(api::addSCOCacheMountPoint(mp3,
                                               std::numeric_limits<uint64_t>::max()));

    EXPECT_THROW(api::addSCOCacheMountPoint(mp1 + "/", 1 << 20),
                 fungi::IOException) <<
        "same mountpoint specified in different flavour must not be accepted";

    EXPECT_THROW(api::removeSCOCacheMountPoint(dummy),
                 fungi::IOException) <<
        "removing an inexistent mountpoint must fail";

    EXPECT_NO_THROW(api::removeSCOCacheMountPoint(mp3));
    EXPECT_NO_THROW(api::removeSCOCacheMountPoint(mp2));
    EXPECT_NO_THROW(api::removeSCOCacheMountPoint(mp1));

    // TODO: try removing the last mountpoint (created in VolManagerTestSetup)
}

TEST_P(ApiTest, SyncSettings)
{
    const std::string volname("volume");
    auto nsptr = make_random_namespace();

    const backend::Namespace& ns = nsptr->ns();

    const VolumeId vol_id(volname);

    newVolume(volname,
              ns);

    uint64_t number_of_syncs_to_ignore;
    uint64_t maximum_time_to_ignore_syncs_in_seconds;

    {
        fungi::ScopedLock l(api::getManagementMutex());

        ASSERT_NO_THROW(api::getSyncIgnore(vol_id,
                                           number_of_syncs_to_ignore,
                                           maximum_time_to_ignore_syncs_in_seconds));

    }

    EXPECT_EQ(0U, number_of_syncs_to_ignore);
    EXPECT_EQ(0U, maximum_time_to_ignore_syncs_in_seconds);

    const uint64_t number_of_syncs_to_ignore_c = 23;
    const uint64_t maximum_time_to_ignore_syncs_in_seconds_c = 127;

    {
        fungi::ScopedLock l(api::getManagementMutex());


        ASSERT_NO_THROW(api::setSyncIgnore(vol_id,
                                           number_of_syncs_to_ignore_c,
                                           maximum_time_to_ignore_syncs_in_seconds_c));
    }

    {
        fungi::ScopedLock l(api::getManagementMutex());


        ASSERT_NO_THROW(api::getSyncIgnore(vol_id,
                                           number_of_syncs_to_ignore,
                                           maximum_time_to_ignore_syncs_in_seconds));
    }

    EXPECT_EQ(number_of_syncs_to_ignore,
              number_of_syncs_to_ignore_c);
    EXPECT_EQ(maximum_time_to_ignore_syncs_in_seconds,
              maximum_time_to_ignore_syncs_in_seconds_c);

}

TEST_P(ApiTest, failOverCacheConfig)
{
    const std::string volname("volume");
    auto nsptr = make_random_namespace();

    const backend::Namespace& ns = nsptr->ns();


    // createTestNamespace(ns);
    const VolumeId vol_id(volname);

    SharedVolumePtr v = newVolume(volname,
                                  ns);
    ASSERT_TRUE(v != nullptr);

    {
        fungi::ScopedLock l(api::getManagementMutex());
        const boost::optional<FailOverCacheConfig>& foc_cfg =
            api::getFailOverCacheConfig(vol_id);

        EXPECT_EQ(boost::none,
                  foc_cfg);
    }

    auto foc_ctx(start_one_foc());

    {
        fungi::ScopedLock l(api::getManagementMutex());
        api::setFailOverCacheConfig(vol_id,
                                    foc_ctx->config(GetParam().foc_mode()));

        const boost::optional<FailOverCacheConfig>& foc_cfg =
            api::getFailOverCacheConfig(vol_id);

        ASSERT_NE(boost::none,
                  foc_cfg);

        EXPECT_EQ(foc_ctx->config(GetParam().foc_mode()),
                  *foc_cfg);
    }
}

TEST_P(ApiTest, concurrentCalls)
{
    boost::condition_variable cond;
    boost::mutex mutex;
    const Namespace nspace;
    const Namespace clone_ns;
    const Namespace nspace2;
    const Namespace nspace3;
    const VolumeId vol_id(nspace.str());
    SnapshotName snap;
    SnapshotName snap2;
    SnapshotName snap3;

    testApi(
            // nspace,
            // clone_ns,
            snap);

    bool run = false;

    boost::thread t1(boost::bind(&ApiTest::fakeRestart,
                                 this,
                                 clone_ns,
                                 &mutex,
                                 &cond,
                                 &run));
    {
        boost::unique_lock<boost::mutex> l(mutex);
        while (not run)
        {
            cond.wait(l);
        }
    }

    boost::thread t2(boost::bind(&ApiTest::testApi,
                                 this,
                                 // nspace2,
                                 // backend::Namespace(),
                                 snap2));

    boost::thread t3(boost::bind(&ApiTest::testApi,
                                 this,
                                 // nspace3,
                                 // backend::Namespace(),
                                 snap3));

    {
        LOCK_MGMT();
        const VolumeId vol_id(clone_ns.str());
        EXPECT_THROW(api::getVolumePointer(vol_id),
                     std::exception);
    }

    {
        LOCK_MGMT();
        EXPECT_THROW(api::local_restart(clone_ns,
                                        new_owner_tag(),
                                        FallBackToBackendRestart::F,
                                        IgnoreFOCIfUnreachable::T),
                     std::exception);
    }

    {
        LOCK_MGMT();
        EXPECT_THROW(api::backend_restart(clone_ns,
                                          new_owner_tag(),
                                          PrefetchVolumeData::F,
                                          IgnoreFOCIfUnreachable::T),
                     std::exception);
    }

    {
        LOCK_MGMT();
        const VolumeSize vol_size(1 << 30);
        EXPECT_THROW(api::createNewVolume(VanillaVolumeConfigParameters(vol_id,
                                                                        clone_ns,
                                                                        vol_size,
                                                                        new_owner_tag())),
                     std::exception);
    }

    {
        LOCK_MGMT();

        const auto params = CloneVolumeConfigParameters(VolumeId(clone_ns.str()),
                                                        clone_ns,
                                                        nspace,
                                                        new_owner_tag())
            .parent_snapshot(snap);

        EXPECT_THROW(api::createClone(params,
                                      PrefetchVolumeData::F),
                     std::exception);
    }

    // add more api calls here - they should all fail as the volume is
    // invisible while restarting

    std::list<VolumeId> vol_list;
    {
        LOCK_MGMT();
        api::getVolumeList(vol_list);
    }

    bool found = false;
    BOOST_FOREACH(VolumeId& i, vol_list)
    {
        if (i == vol_id)
        {
            found = true;
        }
    }

    EXPECT_FALSE(found);

    t2.join();
    t3.join();

    {
        boost::unique_lock<boost::mutex> l(mutex);
        run = false;
    }

    cond.notify_one();
    t1.join();
}

TEST_P(ApiTest, MetaDataStoreMaxPages)
{
    LOCK_MGMT();

    const VolumeId volid("volume");

    const VolumeSize volsize(1ULL << 40);
    const uint32_t max_pages = 1365;

    auto nsidptr = make_random_namespace();

    const Namespace& nsid = nsidptr->ns();

    const OwnerTag otag(new_owner_tag());

    auto params = VanillaVolumeConfigParameters(volid,
                                                nsid,
                                                volsize,
                                                otag)
        .metadata_cache_capacity(max_pages);

    api::createNewVolume(params);

    {
        const MetaDataStoreStats mds = api::getMetaDataStoreStats(volid);
        EXPECT_EQ(max_pages, mds.max_pages);
        EXPECT_EQ(0U, mds.cached_pages);
    }

    api::destroyVolume(volid,
                       DeleteLocalData::F,
                       RemoveVolumeCompletely::F,
                       DeleteVolumeNamespace::F,
                       ForceVolumeDeletion::F);
    api::local_restart(nsid,
                       otag,
                       FallBackToBackendRestart::F,
                       IgnoreFOCIfUnreachable::T);

    {
        const MetaDataStoreStats mds = api::getMetaDataStoreStats(volid);
        EXPECT_EQ(max_pages, mds.max_pages);
        EXPECT_EQ(0U, mds.cached_pages);
    }

    api::destroyVolume(volid,
                       DeleteLocalData::T,
                       RemoveVolumeCompletely::F,
                       DeleteVolumeNamespace::F,
                       ForceVolumeDeletion::F);
    api::backend_restart(nsid,
                         new_owner_tag(),
                         PrefetchVolumeData::F,
                         IgnoreFOCIfUnreachable::T);

    {
        const MetaDataStoreStats mds = api::getMetaDataStoreStats(volid);
        EXPECT_EQ(max_pages, mds.max_pages);
        EXPECT_EQ(0U, mds.cached_pages);
    }
}

TODO("Y42 Enhance this test to check failovercache");

TEST_P(ApiTest, destroyVolumeVariants)
{
    const VolumeId volid("volume");

    const VolumeSize volsize(1ULL << 40);

    const Namespace nsid;
    const OwnerTag otag(new_owner_tag());

    LOCK_MGMT();
    {
        auto nsidptr = make_random_namespace(nsid);

        ASSERT_NO_THROW(api::createNewVolume(VanillaVolumeConfigParameters(volid,
                                                                           nsid,
                                                                           volsize,
                                                                           otag)));

        ASSERT_NO_THROW(api::destroyVolume(volid,
                                           DeleteLocalData::F,
                                           RemoveVolumeCompletely::F,
                                           DeleteVolumeNamespace::F,
                                           ForceVolumeDeletion::F));

        ASSERT_THROW(api::backend_restart(nsid,
                                          otag,
                                          PrefetchVolumeData::F,
                                          IgnoreFOCIfUnreachable::T),
                     fungi::IOException);


        ASSERT_NO_THROW(api::local_restart(nsid,
                                           otag,
                                           FallBackToBackendRestart::F,
                                           IgnoreFOCIfUnreachable::T));

        ASSERT_NO_THROW(api::destroyVolume(volid,
                                           DeleteLocalData::T,
                                           RemoveVolumeCompletely::F,
                                           DeleteVolumeNamespace::F,
                                           ForceVolumeDeletion::F));

        ASSERT_THROW(api::local_restart(nsid,
                                        otag,
                                        FallBackToBackendRestart::F,
                                        IgnoreFOCIfUnreachable::T),
                     fungi::IOException);

        ASSERT_NO_THROW(api::backend_restart(nsid,
                                             otag,
                                             PrefetchVolumeData::F,
                                             IgnoreFOCIfUnreachable::T));

        auto foc_ctx(start_one_foc());

        setFailOverCacheConfig(volid,
                               foc_ctx->config(GetParam().foc_mode()));

        ASSERT_NO_THROW(api::destroyVolume(volid,
                                           DeleteLocalData::T,
                                           RemoveVolumeCompletely::T,
                                           DeleteVolumeNamespace::F,
                                           ForceVolumeDeletion::F));
    }

    ASSERT_THROW(api::local_restart(nsid,
                                    otag,
                                    FallBackToBackendRestart::F,
                                    IgnoreFOCIfUnreachable::T),
                 fungi::IOException);

    ASSERT_THROW(api::backend_restart(nsid,
                                      otag,
                                      PrefetchVolumeData::F,
                                      IgnoreFOCIfUnreachable::T),
                 fungi::IOException);
}

TEST_P(ApiTest, snapshot_metadata)
{
    LOCK_MGMT();

    const VolumeId volid("volume");

    const VolumeSize volsize(1ULL << 40);

    auto nsidptr = make_random_namespace();
    const Namespace nsid = nsidptr->ns();
    const OwnerTag otag(new_owner_tag());

    api::createNewVolume(VanillaVolumeConfigParameters(volid,
                                                       nsid,
                                                       volsize,
                                                       otag));

    WeakVolumePtr vol = api::getVolumePointer(volid);

    const SnapshotName sname1("snapshot");
    // clang analyzer 3.8 does not like using the defaulted default ctor:
    //     default initialization of an object of const type 'const SnapshotMetaData' (aka 'const vector<char>') without a user-provided default constructor
    //     const SnapshotMetaData meta1;
    //                            ^
    //                                 {}
    // 1 error generated.
    const SnapshotMetaData meta1(0);
    api::createSnapshot(volid, meta1, &sname1);

    waitForThisBackendWrite(*SharedVolumePtr(vol));

    {
        const Snapshot snap(api::getSnapshot(volid, sname1));
        EXPECT_TRUE(snap.metadata().empty());
        EXPECT_TRUE(meta1 == snap.metadata());
        EXPECT_EQ(sname1, snap.getName());
    }

    const SnapshotName sname2("snopshat");
    const SnapshotMetaData meta2(sname2.begin(), sname2.end());
    api::createSnapshot(volid, meta2, &sname2);

    waitForThisBackendWrite(*SharedVolumePtr(vol));

    {
        const Snapshot snap(api::getSnapshot(volid, sname2));
        EXPECT_FALSE(snap.metadata().empty());
        EXPECT_TRUE(meta2 == snap.metadata());
        EXPECT_EQ(sname2, snap.getName());
    }

    {
        const SnapshotName sname3("tanshops");
        const SnapshotMetaData meta3(SnapshotPersistor::max_snapshot_metadata_size + 1,
                                     'x');
        EXPECT_THROW(api::createSnapshot(volid, meta3, &sname3),
                     std::exception);
    }

    api::destroyVolume(volid,
                       DeleteLocalData::F,
                       RemoveVolumeCompletely::F,
                       DeleteVolumeNamespace::F,
                       ForceVolumeDeletion::F);

    api::local_restart(nsid,
                       otag,
                       FallBackToBackendRestart::F,
                       IgnoreFOCIfUnreachable::T);

    const Snapshot snap1(api::getSnapshot(volid, sname1));
    EXPECT_TRUE(snap1.metadata().empty());
    EXPECT_TRUE(meta1 == snap1.metadata());
    EXPECT_EQ(sname1, snap1.getName());

    const Snapshot snap2(api::getSnapshot(volid, sname2));
    EXPECT_FALSE(snap2.metadata().empty());
    EXPECT_TRUE(meta2 == snap2.metadata());
    EXPECT_EQ(sname2, snap2.getName());
}

TEST_P(ApiTest, remove_local_data)
{
    const VolumeId vname("volume");

    const VolumeSize vsize(1 << 20);

    auto nspaceptr = make_random_namespace();

    const backend::Namespace nspace = nspaceptr->ns();

    LOCK_MGMT();

    api::createNewVolume(VanillaVolumeConfigParameters(vname,
                                                       nspace,
                                                       vsize,
                                                       new_owner_tag()));

    EXPECT_TRUE(fs::exists(VolManager::get()->getMetaDataPath(nspace)));
    EXPECT_TRUE(fs::exists(VolManager::get()->getTLogPath(nspace)));

    EXPECT_THROW(api::removeLocalVolumeData(nspace),
                 fungi::IOException);

    EXPECT_TRUE(fs::exists(VolManager::get()->getMetaDataPath(nspace)));
    EXPECT_TRUE(fs::exists(VolManager::get()->getTLogPath(nspace)));

    api::destroyVolume(vname,
                       DeleteLocalData::F,
                       RemoveVolumeCompletely::F,
                       DeleteVolumeNamespace::F,
                       ForceVolumeDeletion::F);

    EXPECT_TRUE(fs::exists(VolManager::get()->getMetaDataPath(nspace)));
    EXPECT_TRUE(fs::exists(VolManager::get()->getTLogPath(nspace)));

    api::removeLocalVolumeData(nspace);

    EXPECT_FALSE(fs::exists(VolManager::get()->getMetaDataPath(nspace)));
    EXPECT_FALSE(fs::exists(VolManager::get()->getTLogPath(nspace)));

    api::removeLocalVolumeData(nspace);
}

TEST_P(ApiTest, volume_destruction_vs_monitoring)
{
    const VolumeId vname("volume");

    const VolumeSize vsize(1 << 20);
    WeakVolumePtr v;

    auto nspace_ptr = make_random_namespace();
    const backend::Namespace nspace = nspace_ptr->ns();

    {
        LOCK_MGMT();

        api::createNewVolume(VanillaVolumeConfigParameters(vname,
                                                           nspace,
                                                           vsize,
                                                           new_owner_tag()));

        EXPECT_NO_THROW(api::getMetaDataStoreStats(vname));

        v = api::getVolumePointer(vname);
    }

    const unsigned wait_secs = 60;
    std::thread t;

    // Idea: block the backend and kick off volume destruction in a thread ->
    // volume destruction will get stuck trying to empty the backend queue. At the
    // same time the volume should not be visible to monitoring anymore though to
    // prevent concurrent accesses like the one in OVS-848.
    {
        SCOPED_BLOCK_BACKEND(*SharedVolumePtr(v));

        t = std::thread([&vname]
                        {
                            LOCK_MGMT();

                            api::destroyVolume(vname,
                                               DeleteLocalData::T,
                                               RemoveVolumeCompletely::T,
                                               DeleteVolumeNamespace::F,
                                               ForceVolumeDeletion::F);
                        });

        bool gone = false;

        for (unsigned i = 0; i < (wait_secs * 1000 / 2); ++i)
        {
            std::list<VolumeId> l;

            {
                LOCK_MGMT();
                api::getVolumeList(l);
            }

            if (not l.empty())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            else
            {
                gone = true;
                break;
            }
        }

        ASSERT_TRUE(gone);

        LOCK_MGMT();

        EXPECT_THROW(api::getVolumePointer(vname),
                     std::exception);

        EXPECT_THROW(api::getMetaDataStoreStats(vname),
                     std::exception);
    }

    t.join();
}

TEST_P(ApiTest, snapshot_restoration)
{
    const VolumeId volid("volume");

    const VolumeSize vsize(1 << 20);

    auto nspace_ptr = make_random_namespace();
    const backend::Namespace nspace = nspace_ptr->ns();
    WeakVolumePtr vol;

    {
        LOCK_MGMT();

        api::createNewVolume(VanillaVolumeConfigParameters(volid,
                                                           nspace,
                                                           vsize,
                                                           new_owner_tag()));
        vol = api::getVolumePointer(volid);
    }

    const SnapshotName snapid("snapshot");

    {
        LOCK_MGMT();
        EXPECT_THROW(api::restoreSnapshot(volid, snapid),
                     std::exception);
    }

    const SnapshotName before("before snapshot");

    writeToVolume(*SharedVolumePtr(vol), before);

    {
        LOCK_MGMT();
        api::createSnapshot(volid,
                            SnapshotMetaData(),
                            &snapid);
    }

    const SnapshotName after("after snapshot");

    writeToVolume(*SharedVolumePtr(vol), after);

    waitForThisBackendWrite(*SharedVolumePtr(vol));

    checkVolume(*SharedVolumePtr(vol), after);

    {
        LOCK_MGMT();
        EXPECT_THROW(api::restoreSnapshot(volid,
                                          SnapshotName("no-such-snapshot")),
                     std::exception);
    }

    checkVolume(*SharedVolumePtr(vol), after);

    {
        LOCK_MGMT();
        api::restoreSnapshot(volid, snapid);
    }

    checkVolume(*SharedVolumePtr(vol), before);
}

// A reliable test that volumes are indeed created concurrently and not sequentially
// is tricky, so this test is only concerned with the creation of multiple volumes
// being correct.
TEST_P(ApiTest, concurrent_volume_creation)
{
    const size_t count = 3;

    using WrnsPtr = std::unique_ptr<be::BackendTestSetup::WithRandomNamespace>;
    std::vector<WrnsPtr> nspaces;
    nspaces.reserve(count);

    std::vector<std::future<void>> futures;
    futures.reserve(count);

    for (size_t i = 0; i < count; ++i)
    {
        nspaces.push_back(make_random_namespace());
        futures.push_back(std::async(std::launch::async,
                                     [i,
                                      &nspaces,
                                      this]
                                     {
                                         const be::Namespace& ns(nspaces[i]->ns());
                                         const VolumeId id(ns.str());
                                         const VolumeSize vsize(1ULL << 20);

                                         const VanillaVolumeConfigParameters
                                             params(id,
                                                    ns,
                                                    vsize,
                                                    new_owner_tag());

                                         LOCK_MGMT();
                                         api::createNewVolume(params);
                                     }));
    }

    for (auto& f : futures)
    {
        EXPECT_NO_THROW(f.get());
    }

    for (auto& wrns : nspaces)
    {
        const VolumeId id(wrns->ns().str());

        LOCK_MGMT();
        EXPECT_NO_THROW(SharedVolumePtr(api::getVolumePointer(id)));
    }
}

INSTANTIATE_TEST(ApiTest);

}

// Local Variables: **
// mode: c++ **
// End: **

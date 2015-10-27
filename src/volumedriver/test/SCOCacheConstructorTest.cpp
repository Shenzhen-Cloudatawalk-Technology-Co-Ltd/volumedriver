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

#include <boost/foreach.hpp>

#include "SCOCacheTestSetup.h"

namespace volumedriver
{

namespace yt = youtils;
using namespace initialized_params;

class SCOCacheConstructorTest
    : public SCOCacheTestSetup
{
public:
    SCOCacheConstructorTest()
        : throttling(1000)
    {}

protected:
    virtual void
    SetUp()
    {
        SCOCacheTestSetup::SetUp();
    }

    virtual void
    TearDown()
    {
        SCOCacheTestSetup::TearDown();
    }
    // temporary helper code

    std::atomic<uint32_t> throttling;
};

TEST_F(SCOCacheConstructorTest, invalid)
{
    MountPointConfigs mpCfgs;
    std::auto_ptr<SCOCache> scoCache;
    boost::property_tree::ptree pt;

    PARAMETER_TYPE(datastore_throttle_usecs)(throttling).persist(pt);
    PARAMETER_TYPE(trigger_gap)(yt::DimensionedValue(10ULL<<30)).persist(pt);
    PARAMETER_TYPE(backoff_gap)(yt::DimensionedValue(5ULL << 30)).persist(pt);
    pt.put("version", 1);
    EXPECT_THROW(scoCache.reset(new SCOCache(pt)),
                 fungi::IOException) <<
        "initial mountpoint(s) must be specified";

    const fs::path p(pathPfx_ / "mp");
    mpCfgs.push_back(MountPointConfig(p,
                                      std::numeric_limits<uint64_t>::max()));

    PARAMETER_TYPE(scocache_mount_points)(mpCfgs).persist(pt);


    EXPECT_THROW(scoCache.reset(new SCOCache(pt)),
                 fungi::IOException) <<
        "mountpoint directory must exist";

    fs::create_directories(p);

    EXPECT_THROW(scoCache.reset(new SCOCache(pt)),
                 fungi::IOException) <<
        "trigger gap must not be larger than backoff gap";
    // IMMANUEL -> test should check whether the correct thing is detected
    // AND should eventually not throw.
}

TEST_F(SCOCacheConstructorTest, invalidMountPoint)
{
    MountPointConfigs mpCfgs;

    EXPECT_FALSE(fs::exists(pathPfx_ / "lost+found"));
    EXPECT_FALSE(fs::exists(pathPfx_ / "tooSmall"));
    EXPECT_FALSE(fs::exists(pathPfx_ / "one"));
    EXPECT_FALSE(fs::exists(pathPfx_ / "two"));

    mpCfgs.push_back(MountPointConfig(pathPfx_ / "lost+found", 1 << 30));
    mpCfgs.push_back(MountPointConfig(pathPfx_ / "tooSmall", 0));

    fs::path one(pathPfx_ / "one");
    fs::path two(pathPfx_ / "two");

    fs::create_directories(one);
    fs::create_directories(two);

    mpCfgs.push_back(MountPointConfig(one, 1ULL << 20));
    mpCfgs.push_back(MountPointConfig(two,
                                      std::numeric_limits<uint64_t>::max()));

    std::unique_ptr<SCOCache> scoCache;

    boost::property_tree::ptree pt;

    PARAMETER_TYPE(datastore_throttle_usecs)(throttling).persist(pt);
    PARAMETER_TYPE(trigger_gap)(yt::DimensionedValue(5ULL << 30)).persist(pt);
    PARAMETER_TYPE(backoff_gap)(yt::DimensionedValue(10ULL << 30)).persist(pt);
    pt.put("version", 1);
    PARAMETER_TYPE(scocache_mount_points)(mpCfgs).persist(pt);


    ASSERT_NO_THROW(scoCache.reset(new SCOCache(pt)));

    // EXPECT_TRUE(fs::exists(pathPfx_ / "one"));
    // EXPECT_TRUE(fs::exists(pathPfx_ / "two"));
    // EXPECT_FALSE(fs::exists(pathPfx_ / "tooSmall"));
    // EXPECT_FALSE(fs::exists(pathPfx_ / "lost+found"));

    SCOCacheMountPointList& l = getMountPointList(*scoCache);
    EXPECT_EQ(2U, l.size());

    BOOST_FOREACH(SCOCacheMountPointPtr mp, l)
    {
        EXPECT_TRUE(one == mp->getPath() ||
                    two == mp->getPath()) << mp->getPath();
    }
}

TEST_F(SCOCacheConstructorTest, duplicateMountPoints)
{
    MountPointConfigs mpcfgs;

    const std::string mpname("singlemountpoint");

    fs::path mppath(pathPfx_ / mpname);
    fs::create_directories(mppath);
    mpcfgs.push_back(MountPointConfig(mppath,
                                      std::numeric_limits<uint64_t>::max()));
    boost::property_tree::ptree pt;
    PARAMETER_TYPE(datastore_throttle_usecs)(throttling).persist(pt);
    PARAMETER_TYPE(trigger_gap)(yt::DimensionedValue(5ULL<<30)).persist(pt);
    PARAMETER_TYPE(backoff_gap)(yt::DimensionedValue(10ULL << 30)).persist(pt);
    pt.put("version", 1);

    PARAMETER_TYPE(scocache_mount_points)(mpcfgs).persist(pt);


    std::unique_ptr<SCOCache> scocache(new SCOCache(pt));
    {
        SCOCacheMountPointList& l = getMountPointList(*scocache);
        EXPECT_EQ(1U, l.size());
        EXPECT_EQ(mppath, l.front()->getPath());
    }

    // plain duplicate:
    mpcfgs.push_back(MountPointConfig(mppath,
                                      std::numeric_limits<uint64_t>::max()));

    // more elaborte duplicates:
    mpcfgs.push_back(MountPointConfig(mppath / std::string("../" + mpname),
                                      std::numeric_limits<uint64_t>::max()));

    fs::path symlink(pathPfx_ / "symlink");
    fs::create_symlink(mppath, symlink);
    mpcfgs.push_back(MountPointConfig(symlink,
                                      std::numeric_limits<uint64_t>::max()));

    PARAMETER_TYPE(scocache_mount_points)(mpcfgs).persist(pt);

    // SCOCache::addMountPointsToPropertyTree(mpcfgs,
    //                                        pt);


    scocache.reset(new SCOCache(pt));
    {
        SCOCacheMountPointList& l = getMountPointList(*scocache);
        EXPECT_EQ(1U, l.size());
        EXPECT_EQ(mppath, l.front()->getPath());
    }
}

}
// Local Variables: **
// mode: c++ **
// End: **

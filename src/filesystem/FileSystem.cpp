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

#include "FileSystem.h"
#include "FileSystemEvents.h"
#include "Handle.h"
#include "Registry.h"
#include "TracePoints_tp.h"
#include "VirtualDiskFormat.h"
#include "VirtualDiskFormatVmdk.h"
#include "VirtualDiskFormatRaw.h"
#include "XMLRPC.h"

#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>

#include <dirent.h>
#include <unistd.h>

#include <boost/filesystem/fstream.hpp>
#include <boost/serialization/shared_ptr.hpp>

#include <youtils/Assert.h>
#include <youtils/InlineExecutor.h>
#include <youtils/Logging.h>
#include <youtils/ScopeExit.h>

#include <volumedriver/MDSNodeConfig.h>
#include <volumedriver/MetaDataBackendConfig.h>

namespace volumedriverfs
{

namespace ara = arakoon;
namespace bpt = boost::property_tree;
namespace fs = boost::filesystem;
namespace ip = initialized_params;
namespace vd = volumedriver;
namespace yt = youtils;

#define LOCK_CONFIG()                           \
    boost::lock_guard<decltype(config_lock_)> cfglg__(config_lock_)

namespace
{

DECLARE_LOGGER("FileSystemHelpers");

void
setlimit(int resource, uint32_t value)
{
    if (geteuid() == 0)
    {
        const rlimit rl = { value, value };
        int ret = ::setrlimit(resource, &rl);
        if (ret != 0)
        {
            ret = errno;
            LOG_ERROR("Failed to set rlimit " << resource << ": " <<
                      strerror(ret));
            throw fungi::IOException("failed to set rlimit");
        }
    }
    else
    {
        LOG_WARN("Not running as superuser, not setting limits");
    }
}

std::unique_ptr<VirtualDiskFormat>
get_virtual_disk_format(const bpt::ptree& pt)
{
    const DECLARE_PARAMETER(fs_virtual_disk_format) (pt);
    if (fs_virtual_disk_format.value() == VirtualDiskFormatVmdk::name_)
    {
        return std::unique_ptr<VirtualDiskFormat>(new VirtualDiskFormatVmdk());
    }
    if (fs_virtual_disk_format.value() == VirtualDiskFormatRaw::name_)
    {
        const DECLARE_PARAMETER(fs_raw_disk_suffix) (pt);
        if(fs_raw_disk_suffix.value().empty())
        {
            LOG_ERROR("Cannot accept empty fs_raw_disk_suffix as all files will be seen as volumes");
            throw InvalidConfigurationException("empty fs_raw_disk_suffix");
        }
        return std::unique_ptr<VirtualDiskFormat>(new VirtualDiskFormatRaw(fs_raw_disk_suffix.value()));
    }
    else
    {
        LOG_ERROR("Unknown virtual disk format " << fs_virtual_disk_format.value() << " specified");
        throw InvalidConfigurationException("Unknown virtual disk format",
                                            fs_virtual_disk_format.value().c_str());
    }
}

bool
is_directory(const DirectoryEntryPtr& dentry)
{
    return dentry->type() == DirectoryEntry::Type::Directory;
}

bool
is_volume(const DirectoryEntryPtr& dentry)
{
    return dentry->type() == DirectoryEntry::Type::Volume;
}

boost::optional<vd::FailOverCacheConfig>
make_foc_config(const std::string& host,
                const uint16_t port,
                const vd::FailOverCacheMode mode)
{
    if (host.empty())
    {
        return boost::none;
    }
    else
    {
        return vd::FailOverCacheConfig(host,
                                       port,
                                       mode);
    }
}

}

FileSystem::FileSystem(const bpt::ptree& pt,
                       const RegisterComponent registerizle,
                       const RestartVolumes restart_volumes)
    : VolumeDriverComponent(registerizle,
                            pt)
    , file_event_rules_(PARAMETER_VALUE_FROM_PROPERTY_TREE(fs_file_event_rules, pt))
    , vdisk_format_(get_virtual_disk_format(pt))
    , fs_ignore_sync(pt)
    , fs_internal_suffix(pt)
    , fs_metadata_backend_type(pt)
    , fs_metadata_backend_arakoon_cluster_id(pt)
    , fs_metadata_backend_arakoon_cluster_nodes(pt)
    , fs_metadata_backend_mds_nodes(pt)
    , fs_metadata_backend_mds_apply_relocations_to_slaves(pt)
    , fs_metadata_backend_mds_timeout_secs(pt)
    , fs_metadata_backend_mds_slave_max_tlogs_behind(pt)
    , fs_cache_dentries(pt)
    , fs_nullio(pt)
    , fs_dtl_config_mode(pt)
    , fs_dtl_host(pt)
    , fs_dtl_port(pt)
    , fs_dtl_mode(pt)
    , fs_enable_shm_interface(pt)
    , fs_enable_network_interface(pt)
    , registry_(std::make_shared<Registry>(pt))
    , router_(pt,
              std::static_pointer_cast<yt::LockedArakoon>(registry_),
              fs_dtl_config_mode.value(),
              fs_dtl_mode.value(),
              make_foc_config(fs_dtl_host.value(),
                              fs_dtl_port.value(),
                              fs_dtl_mode.value()))
    , mdstore_(std::static_pointer_cast<yt::LockedArakoon>(registry_),
               router_.cluster_id(),
               fs_cache_dentries.value() ?
               UseCache::T :
               UseCache::F)
    , stats_collector_(pt,
                       registerizle)
    , xmlrpc_svc_(router_.node_config().xmlrpc_host,
                  router_.node_config().xmlrpc_port)
{
    verify_volume_suffix_(fs_internal_suffix.value());

    DECLARE_PARAMETER(fs_max_open_files)(pt);
    setlimit(RLIMIT_NOFILE, fs_max_open_files.value());

    restart_(restart_volumes);

    InstantiateXMLRPCS<xmlrpcs>::doit(xmlrpc_svc_, *this);
    xmlrpc_svc_.start();

    LOG_INFO(name() << " initialized");
}

FileSystem::~FileSystem()
{
    try
    {
        xmlrpc_svc_.stop();
        xmlrpc_svc_.join();
    }
    CATCH_STD_ALL_LOG_IGNORE(name() << ": failed to clean up XMLRPC thread");

    LOG_INFO(name() << ": sayonara");
}

void
FileSystem::destroy(const bpt::ptree& pt)
{
    // This param used to belong exclusively to ObjectRouter (nee VolumeRouter), but
    // since the MetaDataStore also makes use of it and we don't instantiate an
    // ObjectRouter here we have to break this:
    const ClusterId cluster_id(PARAMETER_VALUE_FROM_PROPERTY_TREE(vrouter_cluster_id, pt));

    LOG_INFO(cluster_id << ": removal of all non-local data and metadata requested");

    std::shared_ptr<yt::LockedArakoon> larakoon(new Registry(pt));

    try
    {
        MetaDataStore::destroy(larakoon,
                               cluster_id);
    }
    CATCH_STD_ALL_LOG_RETHROW(cluster_id <<
                              ": failed to remove filesystem metadata - manual intervention required");

    try
    {
        ObjectRouter::destroy(larakoon, pt);
    }
    CATCH_STD_ALL_LOG_RETHROW(cluster_id <<
                              ": failed to remove filesystem - manual intervention required");
}

const char*
FileSystem::componentName() const
{
    return initialized_params::filesystem_component_name;
}

void
FileSystem::update_dtl_settings_(const bpt::ptree& pt,
                                 yt::UpdateReport& rep)
{
#define U(var)                                  \
        (var).update(pt, rep)

    if (fs_dtl_config_mode.value() == FailOverCacheConfigMode::Manual)
    {
        const ip::PARAMETER_TYPE(fs_dtl_host) new_host(pt);
        const ip::PARAMETER_TYPE(fs_dtl_port) new_port(pt);
        const ip::PARAMETER_TYPE(fs_dtl_mode) new_mode(pt);

        try
        {
            router_.set_manual_default_foc_config(make_foc_config(new_host.value(),
                                                                  new_port.value(),
                                                                  new_mode.value()));
        }
        CATCH_STD_ALL_LOG_IGNORE("Failed to apply new DTL settings");
    }

    U(fs_dtl_config_mode);
    U(fs_dtl_host);
    U(fs_dtl_port);
    U(fs_dtl_mode);

#undef U
}

void
FileSystem::update(const bpt::ptree& pt,
                   yt::UpdateReport& rep)
{
    LOCK_CONFIG();

#define U(var)                                  \
        (var).update(pt, rep)

    U(fs_ignore_sync);
    U(fs_internal_suffix);
    U(fs_metadata_backend_type);
    U(fs_metadata_backend_arakoon_cluster_id);
    U(fs_metadata_backend_arakoon_cluster_nodes);
    U(fs_metadata_backend_mds_nodes);
    U(fs_metadata_backend_mds_apply_relocations_to_slaves);
    U(fs_metadata_backend_mds_timeout_secs);
    U(fs_metadata_backend_mds_slave_max_tlogs_behind);
    U(fs_cache_dentries);
    U(fs_nullio);

    update_dtl_settings_(pt, rep);

    U(fs_enable_shm_interface);
    U(fs_enable_network_interface);
    U(ip::PARAMETER_TYPE(fs_virtual_disk_format)(vdisk_format_->name()));
    U(ip::PARAMETER_TYPE(fs_file_event_rules)(file_event_rules_));
#undef U
}

void
FileSystem::persist(bpt::ptree& pt,
                    const ReportDefault report_default) const
{
    LOCK_CONFIG();

#define P(var)                                  \
    (var).persist(pt, report_default)

    P(fs_ignore_sync);
    P(fs_internal_suffix);
    P(fs_metadata_backend_type);
    P(fs_metadata_backend_arakoon_cluster_id);
    P(fs_metadata_backend_arakoon_cluster_nodes);
    P(fs_metadata_backend_mds_nodes);
    P(fs_metadata_backend_mds_apply_relocations_to_slaves);
    P(fs_metadata_backend_mds_timeout_secs);
    P(fs_metadata_backend_mds_slave_max_tlogs_behind);
    P(fs_cache_dentries);
    P(fs_nullio);
    P(fs_dtl_config_mode);
    P(fs_dtl_host);
    P(fs_dtl_port);
    P(fs_enable_shm_interface);
    P(fs_enable_network_interface);

    P(ip::PARAMETER_TYPE(fs_virtual_disk_format)(vdisk_format_->name()));
    P(ip::PARAMETER_TYPE(fs_file_event_rules)(file_event_rules_));
#undef P
}

bool
FileSystem::checkConfig(const bpt::ptree& pt,
                        yt::ConfigurationReport& crep) const
{
    LOCK_CONFIG();

    bool res = true;

    switch (fs_metadata_backend_type.value())
    {
    case vd::MetaDataBackendType::Arakoon:
        {
            ip::PARAMETER_TYPE(fs_metadata_backend_arakoon_cluster_nodes) nodes(pt);
            if (nodes.value().empty())
            {
                crep.push_front(yt::ConfigurationProblem(nodes.name(),
                                                         nodes.section_name(),
                                                         "value must not be empty"));
                res = false;
            }

            ip::PARAMETER_TYPE(fs_metadata_backend_arakoon_cluster_id) cluster_id(pt);
            if (nodes.value().empty())
            {
                crep.push_front(yt::ConfigurationProblem(cluster_id.name(),
                                                         cluster_id.section_name(),
                                                         "value must not be empty"));
                res = false;
            }

            break;
        }
    case vd::MetaDataBackendType::MDS:
        {
            ip::PARAMETER_TYPE(fs_metadata_backend_mds_nodes) nodes(pt);
            if (nodes.value().empty())
            {
                crep.push_front(yt::ConfigurationProblem(nodes.name(),
                                                         nodes.section_name(),
                                                         "value must not be empty"));
                res = false;
            }

            ip::PARAMETER_TYPE(fs_metadata_backend_mds_timeout_secs) timeout_secs(pt);
            if (timeout_secs.value() == 0)
            {
                crep.push_front(yt::ConfigurationProblem(timeout_secs.name(),
                                                         timeout_secs.section_name(),
                                                         "value must not be 0"));
                res = false;
            }

            break;
        }
    case vd::MetaDataBackendType::RocksDB:
    case vd::MetaDataBackendType::TCBT:
        break;
    }

    return res;
}

bool
FileSystem::is_volume_path_(const FrontendPath& p) const
{
    return vdisk_format_->is_volume_path(p);
}

void
FileSystem::restart_(const RestartVolumes restart_volumes)
{
    auto fun([&](const FrontendPath& p, const DirectoryEntryPtr dentry)
             {
                 const ObjectId& id = dentry->object_id();
                 if (not is_directory(dentry))
                 {
                     if (is_volume(dentry) and restart_volumes == RestartVolumes::F)
                     {
                         LOG_INFO(p << ": skipping restart of volume " << id);
                     }
                     else
                     {
                         LOG_TRACE(p << ": trying to restart " << id);
                         try
                         {
                             router_.maybe_restart(id,
                                                   ForceRestart::F);
                         }
                         CATCH_STD_ALL_LOG_IGNORE(p << ": failed to restart volume " <<
                                                  id);
                     }
                 }
             });

    mdstore_.walk(FrontendPath("/"),
                  std::move(fun));
}

void
FileSystem::migrate(const ObjectId& id)
{
    LOG_INFO("migrating " << id);
    router_.migrate(id);
}

void
FileSystem::verify_volume_suffix_(const std::string& sfx)
{
    if (sfx.empty())
    {
        throw fungi::IOException("Suffix must not be empty");
    }

    if (sfx == ".")
    {
        throw fungi::IOException("Suffix '.' is not supported");
    }

    if (sfx[0] != '.')
    {
        throw fungi::IOException("Suffix must start with '.'");
    }

    if (sfx.find('/') != sfx.npos)
    {
        throw fungi::IOException("Suffix must not contain '/'");
    }
}

void
FileSystem::verify_file_path_(const FrontendPath& path)
{
    const fs::path filepath(path.str());
    THROW_WHEN(filepath.empty());
    THROW_UNLESS(filepath.is_absolute());
    THROW_UNLESS(filepath.has_parent_path());
    THROW_WHEN(filepath.filename().empty());

    for (const auto& c : filepath)
    {
        THROW_WHEN(c.string() == ".");
        THROW_WHEN(c.string() == "..");
    }
}

vd::VolumeId
FileSystem::create_volume_or_clone_(const FrontendPath& path,
                                    CreateVolumeOrCloneFun&& fun)
{
    verify_file_path_(path);

    // XXX: This is pretty ugly as it opens a window for a race condition.
    if (mdstore_.find(path))
    {
        LOG_ERROR("Path " << path << " already exists");
        throw FileExistsException("Path already exists",
                                  path.string().c_str(),
                                  EEXIST);
    }

    DirectoryEntryPtr
        dentry(boost::make_shared<DirectoryEntry>(DirectoryEntry::Type::Volume,
                                                  mdstore_.alloc_inode(),
                                                  Permissions(S_IWUSR bitor
                                                              S_IRUSR),
                                                  UserId(::getuid()),
                                                  GroupId(::getgid())));

    LOG_INFO("Trying to create volume or clone " << dentry->object_id() <<
             " @ " << path);

    // If anything beyond that point throws we're screwed as we cannot easily revert the
    // directory creation. We could be a bit smarter and create a temp dir that we rename
    // only on success but this then opens the door for someone else creating entries
    // with the same name as in our target path.
    // Maybe this simply points to us creating intermediate directories not being a
    // good idea?
    mdstore_.add_directories(FrontendPath(path.parent_path()));

    const ObjectId& oid = dentry->object_id();
    const vd::VolumeId id(oid.str());
    vdisk_format_->create_volume(*this,
                                 path,
                                 id,
                                 [&](const FrontendPath& p)
                                 {
                                     VERIFY(is_volume_path_(p));

                                     fun(p,
                                         dentry);

                                     try
                                     {
                                         mdstore_.add(p, dentry);
                                     }
                                     CATCH_STD_ALL_EWHAT({
                                             LOG_ERROR("Failed to persist volume " <<
                                                       id << ", path " <<
                                                       p << ": " << EWHAT);
                                             router_.unlink(ObjectId(id.str()));
                                             throw;
                                         });
                                 });

    const auto ev(FileSystemEvents::volume_create(id,
                                                  path,
                                                  router_.get_size(oid)));
    router_.event_publisher()->publish(ev);

    return id;
}

vd::VolumeId
FileSystem::create_clone(const FrontendPath& clone_path,
                         vd::VolumeConfig::MetaDataBackendConfigPtr mdb_config,
                         const vd::VolumeId& parent_id,
                         const MaybeSnapshotName& maybe_parent_snap)
{
    LOG_INFO("Trying to create clone from parent " << parent_id << ", snapshot " <<
             maybe_parent_snap << " @ " << clone_path);

    if (not mdb_config)
    {
        mdb_config = make_metadata_backend_config();
    }

    return create_volume_or_clone_(clone_path,
                                   [&](const FrontendPath& path,
                                       DirectoryEntryPtr dentry)
                                   {
                                       create_clone_(path,
                                                     std::move(mdb_config),
                                                     parent_id,
                                                     maybe_parent_snap,
                                                     dentry);
                                   });
}

void
FileSystem::create_clone_(const FrontendPath& path,
                          vd::VolumeConfig::MetaDataBackendConfigPtr mdb_config,
                          const vd::VolumeId& parent,
                          const MaybeSnapshotName& maybe_parent_snap,
                          DirectoryEntryPtr dentry)
{
    const vd::VolumeId id(dentry->object_id().str());
    router_.create_clone(id,
                         std::move(mdb_config),
                         parent,
                         maybe_parent_snap);

    LOG_INFO("Create cloned volume " << id << " from parent " << parent <<
             ", snapshot " << maybe_parent_snap << " @ " << path);
}

vd::VolumeId
FileSystem::create_volume(const FrontendPath& path,
                          vd::VolumeConfig::MetaDataBackendConfigPtr mdb_config,
                          uint64_t size)
{
    LOG_INFO("Trying to create volume of size " << size << " @ " << path);

    if (not mdb_config)
    {
        mdb_config = make_metadata_backend_config();
    }

    return create_volume_or_clone_(path,
                                   [&](const FrontendPath& p,
                                       DirectoryEntryPtr dentry)
                                   {
                                       create_volume_(p,
                                                      std::move(mdb_config),
                                                      size,
                                                      dentry);
                                   });
}

void
FileSystem::create_volume_(const FrontendPath& path,
                           vd::VolumeConfig::MetaDataBackendConfigPtr mdb_config,
                           const uint64_t size,
                           DirectoryEntryPtr dentry)
{
    const Object obj(ObjectType::Volume,
                     dentry->object_id());

    router_.create(obj,
                   std::move(mdb_config));

    try
    {
        router_.resize(dentry->object_id(),
                       size);
    }
    CATCH_STD_ALL_EWHAT({
            LOG_ERROR("Failed to resize " << obj << " to " << size << ": " << EWHAT);
            router_.unlink(obj.id);
            throw;
        });

    LOG_INFO("Created " << obj << " of size " << size << " @ " << path);
}

template<typename... A>
void
FileSystem::maybe_publish_file_event_(const FileSystemCall call,
                                      events::Event (*fun)(const FrontendPath&,
                                                           A... args),
                                      const FrontendPath& path,
                                      A... args)
{
    for (const auto& rule : file_event_rules_)
    {
        if (rule.match(call, path))
        {
            const auto ev((*fun)(path, args...));
            router_.event_publisher()->publish(ev);
        }
    }
}

void
FileSystem::maybe_publish_file_rename_(const FrontendPath& from,
                                       const FrontendPath& to)
{
    for (const auto& rule : file_event_rules_)
    {
        if (rule.match(FileSystemCall::Rename, from) or
            rule.match(FileSystemCall::Rename, to))
        {
            auto ev(FileSystemEvents::file_rename(from,
                                                  to));
            router_.event_publisher()->publish(ev);
        }
    }
}

void
FileSystem::opendir(const FrontendPath& path,
                    Handle::Ptr& h)
{
    LOG_TRACE(path);

    DirectoryEntryPtr dentry(mdstore_.find_throw(path));

    if (is_directory(dentry))
    {
        h.reset(new Handle(path,
                           dentry));
    }
    else
    {
        LOG_N_THROW(ENOTDIR,
                    path << " is not a directory");
    }
}

void
FileSystem::releasedir(const FrontendPath& path,
                       Handle::Ptr h)
{
    LOG_TRACE(path);
    THROW_UNLESS(h);
}

std::unique_ptr<vd::MetaDataBackendConfig>
FileSystem::make_metadata_backend_config()
{
    LOCK_CONFIG();

    std::unique_ptr<vd::MetaDataBackendConfig> mdb;

    switch (fs_metadata_backend_type.value())
    {
    case vd::MetaDataBackendType::Arakoon:
        {
            const ara::ClusterID cluster_id(fs_metadata_backend_arakoon_cluster_id.value());
            const auto& configv(fs_metadata_backend_arakoon_cluster_nodes.value());
            mdb.reset(new vd::ArakoonMetaDataBackendConfig(cluster_id,
                                                           configv));
            break;
        }
    case vd::MetaDataBackendType::MDS:
        {
            const auto& configv(fs_metadata_backend_mds_nodes.value());
            const vd::ApplyRelocationsToSlaves
                apply_relocs(fs_metadata_backend_mds_apply_relocations_to_slaves.value() ?
                             vd::ApplyRelocationsToSlaves::T :
                             vd::ApplyRelocationsToSlaves::F);
            boost::optional<uint32_t> max_tlogs_behind;
            if (fs_metadata_backend_mds_slave_max_tlogs_behind.value() != std::numeric_limits<uint32_t>::max())
            {
                max_tlogs_behind = fs_metadata_backend_mds_slave_max_tlogs_behind.value();
            }

            mdb.reset(new vd::MDSMetaDataBackendConfig(configv,
                                                       apply_relocs,
                                                       fs_metadata_backend_mds_timeout_secs.value(),
                                                       max_tlogs_behind));
            break;
        }
    case vd::MetaDataBackendType::RocksDB:
        mdb.reset(new vd::RocksDBMetaDataBackendConfig());
        break;
    case vd::MetaDataBackendType::TCBT:
        mdb.reset(new vd::TCBTMetaDataBackendConfig());
        break;
    }

    return mdb;
}

template<typename ...A>
void
FileSystem::do_mknod(const FrontendPath& path,
                     UserId uid,
                     GroupId gid,
                     Permissions pms,
                     A&&... args)
{
    const bool is_volume = is_volume_path_(path);

    vd::VolumeConfig::MetaDataBackendConfigPtr mdb;
    if (is_volume)
    {
        mdb = make_metadata_backend_config();
    }

    DirectoryEntryPtr
        dentry(boost::make_shared<DirectoryEntry>(is_volume ?
                                                  DirectoryEntry::Type::Volume :
                                                  DirectoryEntry::Type::File,
                                                  mdstore_.alloc_inode(),
                                                  pms,
                                                  uid,
                                                  gid));

    const Object obj(is_volume ?
                     ObjectType::Volume :
                     ObjectType::File,
                     dentry->object_id());

    try
    {
        router_.create(obj,
                       std::move(mdb));
    }
    CATCH_STD_ALL_EWHAT({
            LOG_ERROR("Failed to create object " << obj << " @ " << path <<
                      ", mode " << std::oct << pms << std::dec <<
                      ", uid " << uid << ", gid " << gid << ": " << EWHAT);

            if (is_volume)
            {
                const auto ev(FileSystemEvents::volume_create_failed(path,
                                                                     EWHAT));
                router_.event_publisher()->publish(ev);
            }

            throw;
        });

    try
    {
        mdstore_.add(std::forward<A>(args)...,
                     dentry);
    }
    CATCH_STD_ALL_EWHAT({
            LOG_ERROR("Failed to add object " << obj << " to metadata store: " << EWHAT);
            router_.unlink(obj.id);

            if (is_volume)
            {
                const auto ev(FileSystemEvents::volume_create_failed(path,
                                                                     EWHAT));
                router_.event_publisher()->publish(ev);
            }

            throw;
        })

    if (is_volume)
    {
        const auto ev(FileSystemEvents::volume_create(vd::VolumeId(obj.id.str()),
                                                      path,
                                                      router_.get_size(obj.id)));

        router_.event_publisher()->publish(ev);
    }
    else
    {
        maybe_publish_file_event_(FileSystemCall::Mknod,
                                  &FileSystemEvents::file_create,
                                  path);
    }
}

void
FileSystem::mknod(const FrontendPath& path,
                  UserId uid,
                  GroupId gid,
                  Permissions pms)
{
    LOG_TRACE(path << ": uid " << uid << ", gid " << gid <<
              ", permissions " << std::oct << pms);

    update_parent_mtime(FrontendPath(path.parent_path()));

    do_mknod(path,
             uid,
             gid,
             pms,
             path);
}

void
FileSystem::mknod(const ObjectId& parent_id,
                  const std::string& name,
                  UserId uid,
                  GroupId gid,
                  Permissions pms)
{
    LOG_TRACE(parent_id << ": uid " << uid << ", gid " << gid <<
              ", permissions " << std::oct << pms);

    const FrontendPath path(mdstore_.find_path(parent_id) / name);

    update_parent_mtime(parent_id);

    do_mknod(path,
             uid,
             gid,
             pms,
             parent_id,
             name);
}

template<typename ...A>
void
FileSystem::do_mkdir(UserId uid,
                     GroupId gid,
                     Permissions pms,
                     A&&... args)
{
    DirectoryEntryPtr
        dentry(boost::make_shared<DirectoryEntry>(DirectoryEntry::Type::Directory,
                                                  mdstore_.alloc_inode(),
                                                  pms,
                                                  uid,
                                                  gid));

    mdstore_.add(std::forward<A>(args)...,
                 dentry);
}

void
FileSystem::mkdir(const FrontendPath& path,
                  UserId uid,
                  GroupId gid,
                  Permissions pms)
{
    LOG_TRACE(path << ": uid " << uid << ", gid " << gid << ", permissions " <<
              std::oct << pms);

    update_parent_mtime(FrontendPath(path.parent_path()));

    do_mkdir(uid,
             gid,
             pms,
             path);
}

void
FileSystem::mkdir(const ObjectId& parent_id,
                  const std::string& name,
                  UserId uid,
                  GroupId gid,
                  Permissions pms)
{
    LOG_TRACE(name << ": uid " << uid << ", gid " << gid << ", permissions " <<
              std::oct << pms);

    update_parent_mtime(parent_id);

    do_mkdir(uid,
             gid,
             pms,
             parent_id,
             name);
}

template<typename ...A>
void
FileSystem::do_unlink(const FrontendPath& path,
                      const DirectoryEntryPtr& dentry,
                      A... args)
{
    const ObjectId& id = dentry->object_id();

    if (not is_directory(dentry))
    {
        router_.unlink(id);
    }

    if (is_volume(dentry))
    {
        const auto ev(FileSystemEvents::volume_delete(vd::VolumeId(id.str()),
                                                      path));
        router_.event_publisher()->publish(ev);
    }
    else
    {
        maybe_publish_file_event_(FileSystemCall::Unlink,
                                  &FileSystemEvents::file_delete,
                                  path);
    }

    mdstore_.unlink(args...);
}

void
FileSystem::unlink(const FrontendPath& path)
{
    LOG_TRACE(path);

    DirectoryEntryPtr dentry(mdstore_.find_throw(path));

    update_parent_mtime(FrontendPath(path.parent_path()));

    do_unlink(path,
              dentry,
              path);
}

void
FileSystem::unlink(const ObjectId& parent_id,
                   const std::string& name)
{
    LOG_TRACE(parent_id);

    const FrontendPath path(mdstore_.find_path(parent_id) / name);
    DirectoryEntryPtr dentry(mdstore_.find_throw(path));

    update_parent_mtime(parent_id);

    do_unlink(path,
              dentry,
              parent_id,
              name);
}

void
FileSystem::rmdir(const FrontendPath& path)
{
    LOG_TRACE(path);

    DirectoryEntryPtr dentry(mdstore_.find_throw(path));

    if (is_directory(dentry))
    {
        mdstore_.unlink(path);
    }
    else
    {
        LOG_N_THROW(ENOTDIR,
                    path <<
                    " is not a directory - refusing to remove it");
    }
}

void
FileSystem::rmdir(const ObjectId& id)
{
    LOG_TRACE(id);

    DirectoryEntryPtr dentry(mdstore_.find_throw(id));

    if (is_directory(dentry))
    {
        mdstore_.unlink(FrontendPath(mdstore_.find_path(id)));
    }
    else
    {
        LOG_N_THROW(ENOTDIR,
                    id <<
                    " is not a directory - refusing to remove it");
    }
}

void
FileSystem::rename(const FrontendPath& from,
                   const FrontendPath& to,
                   RenameFlags flags)
{
    LOG_TRACE(from << " -> " << to << ", flags " << std::hex << flags);

    if (flags != RenameFlags::None)
    {
        TODO("AR: support rename flags");

        LOG_N_THROW(EPERM,
                    "We don't support rename flags " << std::hex << flags << " (yet?)");
    }

    DirectoryEntryPtr f(mdstore_.find_throw(from));

    if (is_volume(f))
    {
        LOG_N_THROW(EPERM,
                    "Refusing to rename " << from << " -> " << to <<
                    " as it is a volume (id: " << f->object_id() << ")");
    }

    DirectoryEntryPtr garbage(mdstore_.rename(from, to));
    if (garbage)
    {
        router_.unlink(garbage->object_id());
    }

    maybe_publish_file_rename_(from, to);
}

void
FileSystem::rename(const ObjectId& from_parent_id,
                   const std::string& from,
                   const ObjectId& to_parent_id,
                   const std::string& to,
                   RenameFlags flags)
{
    LOG_TRACE(from << " -> " << to << ", flags " << std::hex << flags);

    if (flags != RenameFlags::None)
    {
        TODO("AR: support rename flags");

        LOG_N_THROW(EPERM,
                    "We don't support rename flags " << std::hex << flags << " (yet?)");
    }

    FrontendPath from_path(mdstore_.find_path(from_parent_id));
    from_path /= from;

    DirectoryEntryPtr f(mdstore_.find_throw(from_path));

    if (is_volume(f))
    {
        LOG_N_THROW(EPERM,
                    "Refusing to rename " << from << " -> " << to <<
                    " as it is a volume (id: " << f->object_id() << ")");
    }

    FrontendPath to_path(mdstore_.find_path(to_parent_id));
    to_path /= to;

    DirectoryEntryPtr garbage(mdstore_.rename(from_parent_id,
                                              from,
                                              to_parent_id,
                                              to));
    if (garbage)
    {
        router_.unlink(garbage->object_id());
    }

    maybe_publish_file_rename_(from_path, to_path);
}

void
FileSystem::truncate(const FrontendPath& path,
                     off_t size)
{
    LOG_TRACE(path << ": size " << size);

    const DirectoryEntryPtr dentry(mdstore_.find_throw(path));

    if (is_directory(dentry))
    {
        LOG_N_THROW(EISDIR,
                    "Refusing to truncate " << path << " which is a directory");
    }

    const ObjectId& id = dentry->object_id();
    router_.resize(id,
                   size);

    if (is_volume(dentry))
    {
        const auto ev(FileSystemEvents::volume_resize(vd::VolumeId(id),
                                                      path,
                                                      size));
        router_.event_publisher()->publish(ev);
    }
}

void
FileSystem::truncate(const ObjectId& id,
                     off_t size)
{
    LOG_TRACE(id << ": size " << size);
    truncate(mdstore_.find_path(id),
             size);
}

void
FileSystem::open(const FrontendPath& path,
                 mode_t openflags,
                 Handle::Ptr& h)
{
    LOG_TRACE(path << ": flags " << std::oct << openflags << std::dec);

    DirectoryEntryPtr dentry(mdstore_.find_throw(path));

    if (is_directory(dentry))
    {
        LOG_N_THROW(EISDIR,
                    "Refusing to open " << path << " as it is a directory");
    }

    LOG_TRACE(path << ": object " << dentry->object_id());
    h.reset(new Handle(path,
                       dentry));
}

void
FileSystem::open(const ObjectId& id,
                 mode_t openflags,
                 Handle::Ptr& h)
{
    LOG_TRACE(id << ": flags " << std::oct << openflags << std::dec);

    open(find_path(id),
         openflags,
         h);
}

void
FileSystem::release(const FrontendPath& p,
                    Handle::Ptr h)
{
    VERIFY(h);
    LOG_TRACE(p);
    release(std::move(h));
}

void
FileSystem::release(Handle::Ptr h)
{
    VERIFY(h);
    LOG_TRACE("Handle " << h.get() << ", path " << h->path());
}

boost::future<size_t>
FileSystem::async_read(Handle& h,
                       size_t size,
                       char* buf,
                       off_t off)
{
    tracepoint(openvstorage_filesystem,
               object_read_start,
               h.dentry()->object_id().str().c_str(),
               off,
               size);

    auto on_exc(yt::make_scope_exit_on_exception([&]
                                                 {
                                                     tracepoint(openvstorage_filesystem,
                                                                object_read_end,
                                                                h.dentry()->object_id().str().c_str(),
                                                                off,
                                                                size,
                                                                true);
                                                 }));

    LOG_TRACE("size " << size << ", off " << off << ", path " << h.path());

    ClusterNode::ReadFuture rf;

    if (fs_nullio.value())
    {
        rf = boost::make_ready_future(size);
    }
    else
    {
        FastPathCookie ck;

        std::tie(ck, rf) = router_.read(h.cookie(),
                                        h.dentry()->object_id(),
                                        reinterpret_cast<uint8_t*>(buf),
                                        size,
                                        off);
        h.update_cookie(ck);
    }

    return rf.then(yt::InlineExecutor::get(),
                   [dentry = h.dentry(),
                    off,
                    size](ClusterNode::ReadFuture f) -> size_t
                   {
                       tracepoint(openvstorage_filesystem,
                                  object_read_end,
                                  dentry->object_id().str().c_str(),
                                  off,
                                  size,
                                  f.has_exception());
                       return f.get();
                   });
}

boost::future<FileSystem::WriteResult>
FileSystem::async_write(Handle& h,
                        size_t size,
                        const char* buf,
                        off_t off)
{
    tracepoint(openvstorage_filesystem,
               object_write_start,
               h.dentry()->object_id().str().c_str(),
               off,
               size,
               false);

    auto on_exc(yt::make_scope_exit_on_exception([&]
                                                 {
                                                     tracepoint(openvstorage_filesystem,
                                                                object_write_end,
                                                                h.dentry()->object_id().str().c_str(),
                                                                off,
                                                                size,
                                                                true);
                                                 }));

    LOG_TRACE("size " << size << ", off " << off <<
              ", handle " << &h << ", path " << h.path() << ", sync " << sync);

    ClusterNode::WriteFuture wf;

    if (fs_nullio.value())
    {
        wf = boost::make_ready_future(std::make_pair(size, vd::DtlInSync::T));
    }
    else
    {
        FastPathCookie c;
        std::tie(c, wf) = router_.write(h.cookie(),
                                        h.dentry()->object_id(),
                                        reinterpret_cast<const uint8_t*>(buf),
                                        size,
                                        off);

        h.update_cookie(std::move(c));

        if (not is_volume(h.dentry()))
        {
            wf = wf.then(yt::InlineExecutor::get(),
                         [path = h.path(),
                          this](ClusterNode::WriteFuture f) -> WriteResult
                         {
                             maybe_publish_file_event_(FileSystemCall::Write,
                                                       &FileSystemEvents::file_write,
                                                       path);
                             return f.get();
                         });
        }
    }

    return wf.then(yt::InlineExecutor::get(),
                   [off,
                    size,
                    dentry = h.dentry()](ClusterNode::WriteFuture f) -> WriteResult
                   {
                       tracepoint(openvstorage_filesystem,
                                  object_write_end,
                                  dentry->object_id().str().c_str(),
                                  off,
                                  size,
                                  f.has_exception());
                       return f.get();
                   });
}

boost::future<vd::DtlInSync>
FileSystem::async_flush(Handle& h,
                        bool datasync)
{
    tracepoint(openvstorage_filesystem,
               object_sync_start,
               h.dentry()->object_id().str().c_str(),
               datasync);

    auto on_exc(yt::make_scope_exit_on_exception([&]
                                                 {
                                                     tracepoint(openvstorage_filesystem,
                                                                object_sync_end,
                                                                h.dentry()->object_id().str().c_str(),
                                                                true);
                                                 }));

    LOG_TRACE("handle " << &h << ", path " << h.path());

    ClusterNode::SyncFuture sf;

    if (fs_nullio.value())
    {
        sf = boost::make_ready_future(vd::DtlInSync::T);
    }
    else
    {
        FastPathCookie c;
        std::tie(c, sf) = router_.sync(h.cookie(),
                                       h.dentry()->object_id());
        h.update_cookie(std::move(c));
    }

    return sf.then(yt::InlineExecutor::get(),
                   [dentry = h.dentry()](ClusterNode::SyncFuture f) -> vd::DtlInSync
                   {
                       tracepoint(openvstorage_filesystem,
                                  object_sync_end,
                                  dentry->object_id().str().c_str(),
                                  f.has_exception());
                       return f.get();
                   });
}

TODO("Phase out get_volume_id in favour of get_object_id");
boost::optional<vd::VolumeId>
FileSystem::get_volume_id(const FrontendPath& path)
{
    DirectoryEntryPtr dentry(mdstore_.find(path));
    if (dentry and is_volume(dentry))
    {
        const vd::VolumeId id(dentry->object_id().str());
        return id;
    }
    else
    {
        return boost::none;
    }
}

boost::optional<ObjectId>
FileSystem::get_object_id(const FrontendPath& path)
{
    DirectoryEntryPtr dentry(mdstore_.find(path));
    if (dentry and not is_directory(dentry))
    {
        return dentry->object_id();
    }
    else
    {
        return boost::none;
    }
}

FrontendPath
FileSystem::find_path(const ObjectId& id)
{
    return FrontendPath(mdstore_.find_path(id));
}

boost::optional<ObjectId>
FileSystem::find_id(const FrontendPath& p)
{
    DirectoryEntryPtr dentry(mdstore_.find(p));
    if (dentry)
    {
        const  ObjectId id(dentry->object_id());
        return id;
    }
    else
    {
        return boost::none;
    }
}

void
FileSystem::drop_from_cache(const FrontendPath& p)
{
    mdstore_.drop_from_cache(p);
}

void
FileSystem::vaai_copy(const FrontendPath& src_path,
                      const FrontendPath& dst_path,
                      const uint64_t& timeout,
                      const CloneFileFlags& flags)
{
    const DirectoryEntryPtr src_dentry(mdstore_.find(src_path));
    DirectoryEntryPtr dst_dentry(mdstore_.find(dst_path));
    const bool is_lazy = flags & CloneFileFlags::Lazy;
    const bool is_guarded = flags & CloneFileFlags::Guarded;
    const bool is_skipzeroes = flags & CloneFileFlags::SkipZeroes;

    if (src_dentry)
    {
        switch (src_dentry->type())
        {
        case DirectoryEntry::Type::Volume:
        {
            if (not is_lazy && not is_guarded && is_skipzeroes &&
                    not dst_dentry)
            {
                throw ObjectNotRegisteredException() <<
                    error_desc("target volume for full clone doesn't exist'n");
            }
            else if (is_lazy && is_guarded && dst_dentry)
            {
                throw FileExistsException("target volume for lazy snapshot exists");
            }
            using MaybeObjectId = boost::optional<ObjectId>;

            vd::VolumeConfig::MetaDataBackendConfigPtr mdb_config(make_metadata_backend_config());
            router_.vaai_copy(src_dentry->object_id(),
                              dst_dentry ?
                              MaybeObjectId(dst_dentry->object_id()) :
                              MaybeObjectId(),
                              std::move(mdb_config),
                              timeout,
                              flags,
                              [&](const MaybeSnapshotName& snapname,
                                  vd::VolumeConfig::MetaDataBackendConfigPtr clone_config){
                                    TODO("cnanakos: Fix create_clone()");
                                    std::string token = dst_path.str().substr(0,
                                                                dst_path.str().find("-flat.vmdk"));
                                    const FrontendPath clone_dst_path(token + ".vmdk");
                                    create_clone(clone_dst_path,
                                                 std::move(clone_config),
                                                 vd::VolumeId(src_dentry->object_id().str()),
                                                 snapname);
                              });
        }
        break;
        case DirectoryEntry::Type::File:
        {
            if (is_lazy && is_guarded && not is_volume_path_(dst_path))
            {
                if (not dst_dentry)
                {
                    mknod(dst_path,
                          UserId(::getuid()),
                          GroupId(::getgid()),
                          Permissions(S_IRUSR bitor S_IWUSR));
                }
                dst_dentry = mdstore_.find(dst_path);
                router_.vaai_filecopy(src_dentry->object_id(),
                                      dst_dentry->object_id());
            }
            else
            {
                throw InvalidOperationException() <<
                    error_desc("unknown file-based VAAI call");
            }
        }
        break;
        default:
            throw InvalidOperationException() <<
                error_desc("Invalid file type");
        }
    }
    else
    {
        throw ObjectNotRegisteredException() <<
            error_desc("source object doesn't exist");
    }
}

ClientInfoTag
FileSystem::register_client(ClientInfo info)
{
    std::lock_guard<std::mutex> lock(client_info_lock_);
    static ClientInfoTag ci_tag_cnt(0);
    const ClientInfoTag tag(++ci_tag_cnt);
    client_info_map_.emplace(tag,
                             std::move(info));
    return tag;
}

void
FileSystem::unregister_client(ClientInfoTag tag)
{
    std::lock_guard<std::mutex> lock(client_info_lock_);
    client_info_map_.erase(tag);
}

std::vector<ClientInfo>
FileSystem::list_registered_clients()
{
    std::vector<ClientInfo> info_vec_;
    std::lock_guard<std::mutex> lock(client_info_lock_);
    for (const auto& kv: client_info_map_)
    {
        info_vec_.push_back(kv.second);
    }
    return info_vec_;
}

void
FileSystem::set_dtl_in_sync(const Handle& h,
                            const vd::DtlInSync dtl_in_sync)
{
    router_.set_dtl_in_sync(h.dentry()->object_id(),
                            dtl_in_sync);
}

}

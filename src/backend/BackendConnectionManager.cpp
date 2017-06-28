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

#include "Alba_Connection.h"
#include "BackendConfig.h"
#include "BackendConnectionManager.h"
#include "BackendInterface.h"
#include "BackendParameters.h"
#include "BackendSinkInterface.h"
#include "Local_Connection.h"
#include "Local_Sink.h"
#include "Local_Source.h"
#include "ManagedBackendSink.h"
#include "ManagedBackendSource.h"
#include "MultiConfig.h"
#include "S3_Connection.h"

#include <numeric>

#include <boost/iostreams/stream.hpp>
#include <boost/thread/thread.hpp>

#include <youtils/Assert.h>

namespace backend
{

namespace bc = boost::chrono;
namespace bpt = boost::property_tree;
namespace bio = boost::iostreams;
namespace ip = initialized_params;
namespace yt = youtils;

BackendConnectionManager::BackendConnectionManager(const bpt::ptree& pt,
                                                   const RegisterComponent registerize)
    : VolumeDriverComponent(registerize,
                            pt)
    , params_(pt)
    , config_(BackendConfig::makeBackendConfig(pt))
{
    THROW_UNLESS(config_);

    if (config_->backend_type.value() == BackendType::MULTI)
    {
        auto& cfg = dynamic_cast<const MultiConfig&>(*config_);
        connection_pools_.reserve(cfg.configs_.size());
        for (const auto& c : cfg.configs_)
        {
            connection_pools_.push_back(ConnectionPool::create(c->clone(),
                                                               params_.backend_connection_pool_capacity.value()));
        }
    }
    else
    {
        connection_pools_.push_back(ConnectionPool::create(config_->clone(),
                                                           params_.backend_connection_pool_capacity.value()));
    }

    THROW_WHEN(connection_pools_.empty());
}

BackendConnectionManagerPtr
BackendConnectionManager::create(const boost::property_tree::ptree& pt,
                                 const RegisterComponent registrate)
{
    return std::make_shared<yt::EnableMakeShared<BackendConnectionManager>>(pt,
                                                                            registrate);
}

const std::shared_ptr<ConnectionPool>&
BackendConnectionManager::pool_(const Namespace& nspace)
{
    ASSERT(not connection_pools_.empty());
    const size_t h = std::hash<std::string>()(nspace.str());
    const size_t idx = h % connection_pools_.size();
    size_t i = idx;
    const bc::seconds timeout(params_.backend_connection_pool_blacklist_secs.value());
    const auto now(ConnectionPool::Clock::now());

    while (true)
    {
        ASSERT(i < connection_pools_.size());
        if (connection_pools_[i]->last_error() + timeout < now)
        {
            break;
        }
        else
        {
            ++i;
            if (i == connection_pools_.size())
            {
                i = 0;
            }

            if (i == idx)
            {
                LOG_ERROR("all pools are blacklisted, picking a random one");
                i = rand_(connection_pools_.size() - 1);
                break;
            }
        }
    }

    return connection_pools_[i];
}

BackendConnectionInterfacePtr
BackendConnectionManager::getConnection(const ForceNewConnection force_new,
                                        const boost::optional<Namespace>& nspace)
{
    ASSERT(not connection_pools_.empty());
    if (nspace)
    {
        return pool_(*nspace)->get_connection(force_new);
    }
    else
    {
        return connection_pools_[0]->get_connection(force_new);
    }
}

size_t
BackendConnectionManager::capacity() const
{
    return std::accumulate(connection_pools_.begin(),
                           connection_pools_.end(),
                           0,
                           [](size_t n, const std::shared_ptr<ConnectionPool>& p)
                           {
                               return n + p->capacity();
                           });
}

size_t
BackendConnectionManager::size() const
{
    return std::accumulate(connection_pools_.begin(),
                           connection_pools_.end(),
                           0,
                           [](size_t n, const std::shared_ptr<ConnectionPool>& p)
                           {
                               return n + p->size();
                           });
}

size_t
BackendConnectionManager::pools() const
{
    return connection_pools_.size();
}

BackendInterfacePtr
BackendConnectionManager::newBackendInterface(const Namespace& nspace)
{
    return BackendInterfacePtr(new BackendInterface(nspace,
                                                    shared_from_this()));
}

std::unique_ptr<BackendSinkInterface>
BackendConnectionManager::newBackendSink(const Namespace& nspace,
                                         const std::string& name)
{
    switch (config_->backend_type.value())
    {
    case BackendType::LOCAL:
    case BackendType::MULTI:
        {
            BackendConnectionInterfacePtr bc = getConnection();
            std::unique_ptr<local::Connection>
                c(dynamic_cast<local::Connection*>(bc.get()));
            VERIFY(c.get() != 0);
            bc.release();
            return std::unique_ptr<BackendSinkInterface>(new local::Sink(std::move(c),
                                                                         nspace,
                                                                         name,
                                                                         boost::posix_time::seconds(0)));
        }
    case BackendType::S3:
        {
            LOG_FATAL("The S3 backend does not support output streaming");
            throw BackendNotImplementedException();
        }
    case BackendType::ALBA:
        {
            LOG_FATAL("The ALBA backend does not support output streaming");
            throw BackendNotImplementedException();
        }

    }
    UNREACHABLE
}

std::unique_ptr<BackendSourceInterface>
BackendConnectionManager::newBackendSource(const Namespace& nspace,
                                           const std::string& name)
{
    switch (config_->backend_type.value())
    {
    case BackendType::LOCAL:
    case BackendType::MULTI:
        {
            BackendConnectionInterfacePtr bc = getConnection();
            std::unique_ptr<local::Connection>
                c(dynamic_cast<local::Connection*>(bc.get()));
            VERIFY(c.get() != 0);
            bc.release();
            return std::unique_ptr<BackendSourceInterface>(new local::Source(std::move(c),
                                                                             nspace,
                                                                             name,
                                                                             boost::posix_time::seconds(0)));
        }
    case BackendType::S3:
        LOG_FATAL("The S3 backend does not support input streaming");
        throw BackendFatalException();
        // return std::unique_ptr<BackendSourceInterface>();

    case BackendType::ALBA:
        LOG_FATAL("The ALBA backend does not support input streaming");
        throw BackendFatalException();

    }

    UNREACHABLE
}

std::unique_ptr<std::ostream>
BackendConnectionManager::getOutputStream(const Namespace& nspace,
                                          const std::string& name,
                                          size_t bufsize)
{
    ManagedBackendSink sink(shared_from_this(), nspace, name);
    return std::unique_ptr<std::ostream>(new bio::stream<ManagedBackendSink>(sink,
                                                                             bufsize));
}

std::unique_ptr<std::istream>
BackendConnectionManager::getInputStream(const Namespace& nspace,
                                         const std::string& name,
                                         size_t bufsize)
{
    ManagedBackendSource source(shared_from_this(), nspace, name);
    return std::unique_ptr<std::istream>(new bio::stream<ManagedBackendSource>(source,
                                                                               bufsize));
}

void
BackendConnectionManager::persist(bpt::ptree& pt,
                                  const ReportDefault report_default) const
{
    config_->persist_internal(pt,
                              report_default);
    params_.persist(pt,
                    report_default);
}

void
BackendConnectionManager::update(const bpt::ptree& pt,
                                 yt::UpdateReport& report)
{
    config_->update_internal(pt, report);

    const decltype(params_.backend_connection_pool_capacity) new_cap(pt);

    for (auto& p : connection_pools_)
    {
        p->capacity(new_cap.value());
    }

    params_.update(pt,
                   report);
}

bool
BackendConnectionManager::checkConfig(const bpt::ptree& pt,
                                      yt::ConfigurationReport& rep) const
{
    return config_->checkConfig_internal(pt,
                                         rep);
}

const char*
BackendConnectionManager::componentName() const
{
    return ip::backend_connection_manager_name;
}

}

// Local Variables: **
// mode: c++ **
// End: **

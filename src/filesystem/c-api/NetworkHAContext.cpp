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

#include "common_priv.h"
#include "Logger.h"
#include "NetworkHAContext.h"
#include "NetworkXioContext.h"
#include "IOThread.h"
#include "Utils.h"

#include <atomic>
#include <memory>
#include <thread>
#include <chrono>

#include <youtils/System.h>
#include <youtils/StringUtils.h>

#define LOCK_INFLIGHT()                                 \
    std::lock_guard<std::mutex> ifrl_(inflight_reqs_lock_)

#define LOCK_SEEN()                                     \
    std::lock_guard<std::mutex> srl_(seen_reqs_lock_)

namespace libovsvolumedriver
{

namespace
{

using namespace std::chrono_literals;

namespace yt = youtils;

using Clock = std::chrono::steady_clock;

std::string ha_hdlr_intvl("LIBOVSVOLUMEDRIVER_HA_HANDLER_INTERVAL_MSECS");
std::string loc_chk_intvl("LIBOVSVOLUMEDRIVER_LOCATION_CHECK_INTERVAL_SECS");
std::string top_chk_intvl("LIBOVSVOLUMEDRIVER_TOPOLOGY_CHECK_INTERVAL_SECS");

const Clock::duration ha_handler_sleep_time =
    std::chrono::milliseconds(yt::System::get_env_with_default(ha_hdlr_intvl,
                                                               5UL));
const Clock::duration location_check_interval =
    std::chrono::seconds(yt::System::get_env_with_default(loc_chk_intvl,
                                                          30UL));
const Clock::duration topology_check_interval =
    std::chrono::seconds(yt::System::get_env_with_default(top_chk_intvl,
                                                          300UL));
}

MAKE_EXCEPTION(NetworkHAContextMemPoolException, fungi::IOException);

NetworkHAContext::NetworkHAContext(const std::string& uri,
                                   uint64_t net_client_qdepth,
                                   bool ha_enabled)
    : uri_(uri)
    , qd_(net_client_qdepth)
    , ha_enabled_(ha_enabled)
    , request_id_(0)
    , opened_(false)
    , openning_(true)
    , connection_error_(false)
    , ctx_(std::make_shared<NetworkXioContext>(uri,
                                               net_client_qdepth,
                                               *this))
{
    LIBLOGID_INFO("uri: " << uri <<
                  ",queue depth: " << net_client_qdepth);
    mpool = std::shared_ptr<xio_mempool>(
                    xio_mempool_create(-1,
                                       XIO_MEMPOOL_FLAG_REGULAR_PAGES_ALLOC),
                    xio_mempool_destroy);
    if (mpool == nullptr)
    {
        LIBLOGID_ERROR("failed to create memory pool");
        throw NetworkHAContextMemPoolException("failed to create memory pool");
    }

    (void) xio_mempool_add_slab(mpool.get(),
                                4096,
                                0,
                                2048,
                                32,
                                0);
    (void) xio_mempool_add_slab(mpool.get(),
                                32768,
                                0,
                                2048,
                                32,
                                0);
    (void) xio_mempool_add_slab(mpool.get(),
                                131072,
                                0,
                                1024,
                                32,
                                0);

    if (ha_enabled)
    {
        try
        {
            ha_ctx_thread_.iothread_ =
                std::thread(&NetworkHAContext::ha_ctx_handler,
                            this,
                            reinterpret_cast<void*>(&ha_ctx_thread_));
        }
        catch (const std::system_error& e)
        {
            LIBLOG_ERROR("cannot create HA handler thread: " << e.what());
            throw;
        }
    }
}

NetworkHAContext::~NetworkHAContext()
{
    ctx_.reset();
}

std::string
NetworkHAContext::get_next_cluster_uri()
{
    assert(not cluster_nw_uris_.empty());
    std::string uri = cluster_nw_uris_.back();
    cluster_nw_uris_.pop_back();
    return uri;
}

void
NetworkHAContext::current_uri(const std::string& u)
{
    std::lock_guard<decltype(config_lock_)> g(config_lock_);
    uri_ = u;
}

std::string
NetworkHAContext::current_uri() const
{
    std::lock_guard<decltype(config_lock_)> g(config_lock_);
    return uri_;
}

void
NetworkHAContext::volume_name(const std::string& n)
{
    std::lock_guard<decltype(config_lock_)> g(config_lock_);
    volume_name_ = n;
}

std::string
NetworkHAContext::volume_name() const
{
    std::lock_guard<decltype(config_lock_)> g(config_lock_);
    return volume_name_;
}

uint64_t
NetworkHAContext::assign_request_id(ovs_aio_request *request)
{
    request->_id = ++request_id_;
    return request->_id;
}

void
NetworkHAContext::insert_inflight_request(const uint64_t id,
                                          ovs_aio_request* req,
                                          NetworkXioContextPtr ctx)
{
    inflight_ha_reqs_.emplace(id, std::make_pair(req, std::move(ctx)));
}

void
NetworkHAContext::fail_inflight_requests(int errval)
{
    LOCK_INFLIGHT();
    remove_all_seen_requests();
    for (auto req_it = inflight_ha_reqs_.cbegin();
            req_it != inflight_ha_reqs_.cend();)
    {
        ovs_aio_request::handle_xio_request(req_it->second.first,
                                            -1,
                                            errval,
                                            false);
        inflight_ha_reqs_.erase(req_it++);
    }
}

void
NetworkHAContext::insert_seen_request(uint64_t id)
{
    LOCK_SEEN();
    seen_ha_reqs_.push(id);
}

void
NetworkHAContext::remove_seen_requests()
{
    std::queue<uint64_t> q_;
    {
        LOCK_SEEN();
        seen_ha_reqs_.swap(q_);
    }
    while (not q_.empty())
    {
        uint64_t id = q_.front();
        q_.pop();
        {
            LOCK_INFLIGHT();
            auto ret = inflight_ha_reqs_.erase(id);
            if (ret == 0)
            {
                LIBLOG_ERROR("inflight request does not exist.");
                assert(ret == 1);
            }
        }
    }
}

void
NetworkHAContext::remove_all_seen_requests()
{
    LOCK_SEEN();
    while (not seen_ha_reqs_.empty())
    {
        uint64_t id = seen_ha_reqs_.front();
        seen_ha_reqs_.pop();
        auto ret = inflight_ha_reqs_.erase(id);
        if (ret == 0)
        {
            LIBLOG_ERROR("inflight request does not exist.");
            assert(ret == 1);
        }
    }
}

void
NetworkHAContext::resend_inflight_requests()
{
    LIBLOGID_INFO("sending inflight requests to URI '" << uri_
                  << "' for volume '" << volume_name_ << "'");
    for (auto& req_entry: inflight_ha_reqs_)
    {
        auto request = req_entry.second.first;
        NetworkXioContextPtr ctx = atomic_get_ctx();

        switch (request->_op)
        {
        case RequestOp::Read:
            ctx->send_read_request(request);
            break;
        case RequestOp::Write:
            ctx->send_write_request(request);
            break;
        case RequestOp::Flush:
        case RequestOp::AsyncFlush:
            ctx->send_flush_request(request);
            break;
        default:
            LIBLOGID_ERROR("unknown inflight request, op:"
                           << static_cast<int>(request->_op));
        }

        req_entry.second.second = ctx;
    }
    LIBLOGID_INFO("inflight requests have been resend to URI '" << uri_
                  << "' for volume '" << volume_name_ << "'");
}

int
NetworkHAContext::do_reconnect(const std::string& uri)
{
    int ret = -1;

    try
    {
        auto tmp_ctx = std::make_shared<NetworkXioContext>(uri,
                                                           qd_,
                                                           *this);
        ret = tmp_ctx->open_volume(volume_name_.c_str(),
                                   oflag);
        if (ret < 0)
        {
            LIBLOGID_ERROR("failed to open " << volume_name_ << " at " << uri);
        }
        else
        {
            atomic_xchg_ctx(tmp_ctx);
            current_uri(uri);
            update_cluster_node_uri();
        }
    }
    catch (std::exception& e)
    {
        LIBLOGID_ERROR("failed to create new context for volume " <<
                       volume_name_ << " at " << uri << ":" << e.what());
    }
    catch (...)
    {
        LIBLOGID_ERROR("failed to create new context for volume " <<
                       volume_name_ << " at " << uri << ": unknown exception");
    }

    return ret;
}

int
NetworkHAContext::reconnect()
{
    int r = -1;

    if (not is_dtl_in_sync())
    {
        LIBLOGID_ERROR("not attempting to failover as the DTL is "
                       "known not to be in sync");
        return r;
    }

    while (not cluster_nw_uris_.empty())
    {
        std::string uri = get_next_cluster_uri();
        if (uri == current_uri())
        {
            continue;
        }

        r = do_reconnect(uri);
        if (r < 0)
        {
            LIBLOGID_ERROR("reconnection to URI '" << uri << "' failed");
        }
        else
        {
            LIBLOGID_INFO("reconnection to URI '" << uri << "' succeeded");
            break;
        }
    }
    return r;
}

void
NetworkHAContext::try_to_reconnect()
{
    LOCK_INFLIGHT();
    remove_all_seen_requests();
    int r = reconnect();
    if (r < 0)
    {
        opened_ = false;
    }
    else
    {
        connection_error_ = false;
        resend_inflight_requests();
    }
}

void
NetworkHAContext::update_cluster_node_uri()
{
    std::vector<std::string> uris;
    int rl = list_cluster_node_uri(uris);
    if (rl < 0)
    {
        LIBLOGID_ERROR("failed to update cluster node URI list: "
                       << yt::safe_error_str(errno));
    }
    else
    {
        LIBLOGID_INFO("cluster node URIs for " << volume_name() << ":");
        for (const auto& u : uris)
        {
            LIBLOGID_INFO("\t" << u);
        }

        std::reverse(uris.begin(), uris.end());
        std::swap(uris, cluster_nw_uris_);
    }
}

// Must not run concurrently to HA - currently this is guaranteed by
// being run only by the HA thread.
void
NetworkHAContext::maybe_update_volume_location()
{
    LIBLOGID_INFO(volume_name() << ": check if location has changed");
    if (not volume_name().empty())
    {
        std::string uri;
        int ret = atomic_get_ctx()->get_volume_uri(volume_name_.c_str(),
                                                   uri);
        if (ret < 0)
        {
            LIBLOGID_ERROR("failed to retrieve location of " << volume_name_
                           << ": " << yt::safe_error_str(errno));
        }
        else if (uri != current_uri())
        {
            LIBLOGID_INFO("location of " << volume_name_ <<
                          " has changed from " << current_uri() << " to " <<
                          uri << " - trying to adjust");
            do_reconnect(uri);
        }
    }
}

void
NetworkHAContext::ha_ctx_handler(void *arg)
{
    IOThread *thread = reinterpret_cast<IOThread*>(arg);
    auto last_location_check = Clock::time_point::min();
    auto last_topology_check = last_location_check;

    const bool check_location = location_check_interval.count() != 0;
    const bool check_topology = topology_check_interval.count() != 0;

    while (not thread->stopping)
    {
        if (is_volume_openning())
        {
            std::this_thread::sleep_for(ha_handler_sleep_time);
            continue;
        }
        if (is_connection_error())
        {
            if (is_volume_open())
            {
                try_to_reconnect();
            }
            else
            {
                fail_inflight_requests(EIO);
            }

            last_location_check = Clock::now();
            last_topology_check = last_location_check;
        }
        else
        {
            remove_seen_requests();

            if (check_location and
                last_location_check + location_check_interval <= Clock::now())
            {
                maybe_update_volume_location();
                last_location_check = Clock::now();
            }

            if (check_topology and
                last_topology_check + topology_check_interval <= Clock::now())
            {
                update_cluster_node_uri();
                last_topology_check = Clock::now();
            }
        }

        std::this_thread::sleep_for(ha_handler_sleep_time);
    }
    std::lock_guard<std::mutex> lock_(thread->mutex_);
    thread->stopped = true;
    thread->cv_.notify_all();
}

int
NetworkHAContext::find_volume_uri_and_open(const char *volname,
                                           int oflag)
{
    volume_name(volname);
    if (is_ha_enabled())
    {
        std::string uri;
        int r = get_volume_uri(volname, uri);
        if (r < 0)
        {
            LIBLOGID_ERROR("failed to find volume uri: " << volname <<
                           ", error: " << yt::safe_error_str(errno));
            return r;
        }
        else if (uri != current_uri())
        {
            r = do_reconnect(uri);
            if (r < 0)
            {
                return atomic_get_ctx()->open_volume(volname,
                                                     oflag);
            }
            else
            {
                return r;
            }
        }
        else
        {
            return atomic_get_ctx()->open_volume(volname,
                                                 oflag);
        }
    }
    else
    {
        return atomic_get_ctx()->open_volume(volname,
                                             oflag);
    }
}

int
NetworkHAContext::open_volume(const char *volname,
                              int oflag)
{
    LIBLOGID_DEBUG("volume name: " << volname << ", oflag: " << oflag);
    int r = find_volume_uri_and_open(volname, oflag);
    if (r < 0)
    {
        LIBLOGID_ERROR("failed to open volume: " << volname << ", error: "
                       << yt::safe_error_str(errno));
    }
    else
    {
        oflag_ = oflag;
        openning_ = false;
        opened_ = true;
        if (is_ha_enabled())
        {
            update_cluster_node_uri();
        }
    }
    return r;
}

void
NetworkHAContext::close_volume()
{
    if (is_ha_enabled())
    {
        ha_ctx_thread_.stop();
        ha_ctx_thread_.reset_iothread();
    }
    atomic_get_ctx()->close_volume();
}

int
NetworkHAContext::create_volume(const char *volume_name,
                                uint64_t size)
{
    return atomic_get_ctx()->create_volume(volume_name, size);
}

int
NetworkHAContext::remove_volume(const char *volume_name)
{
    return atomic_get_ctx()->remove_volume(volume_name);
}

int
NetworkHAContext::truncate_volume(const char *volume_name,
                                  uint64_t size)
{
    return atomic_get_ctx()->truncate_volume(volume_name, size);
}

int
NetworkHAContext::truncate(uint64_t size)
{
    return atomic_get_ctx()->truncate(size);
}

int
NetworkHAContext::snapshot_create(const char *volume_name,
                                  const char *snapshot_name,
                                  const uint64_t timeout)
{
    return atomic_get_ctx()->snapshot_create(volume_name,
                                             snapshot_name,
                                             timeout);
}

int
NetworkHAContext::snapshot_rollback(const char *volume_name,
                                    const char *snapshot_name)
{
    return atomic_get_ctx()->snapshot_rollback(volume_name,
                                               snapshot_name);
}

int
NetworkHAContext::snapshot_remove(const char *volume_name,
                                  const char *snapshot_name)
{
    return atomic_get_ctx()->snapshot_remove(volume_name,
                                             snapshot_name);
}

void
NetworkHAContext::list_snapshots(std::vector<std::string>& snaps,
                                 const char *volume_name,
                                 uint64_t *size,
                                 int *saved_errno)
{
    atomic_get_ctx()->list_snapshots(snaps,
                                     volume_name,
                                     size,
                                     saved_errno);
}

int
NetworkHAContext::is_snapshot_synced(const char *volume_name,
                                     const char *snapshot_name)
{
    return atomic_get_ctx()->is_snapshot_synced(volume_name,
                                                snapshot_name);
}

int
NetworkHAContext::list_volumes(std::vector<std::string>& volumes)
{
    return atomic_get_ctx()->list_volumes(volumes);
}

int
NetworkHAContext::list_cluster_node_uri(std::vector<std::string>& uris)
{
    return atomic_get_ctx()->list_cluster_node_uri(uris);
}

int
NetworkHAContext::get_volume_uri(const char* volume_name,
                                 std::string& uri)
{
    return atomic_get_ctx()->get_volume_uri(volume_name,
                                            uri);
}

template<typename... Args>
int
NetworkHAContext::wrap_io(int (NetworkXioContext::*mem_fun)(ovs_aio_request*,
                                                            Args...),
                          ovs_aio_request* request,
                          Args... args)
{
    int r;

    assert(request->_op != RequestOp::Noop);
    assert(request->_op != RequestOp::Open);
    assert(request->_op != RequestOp::Close);

    if (is_ha_enabled())
    {
        uint64_t id = assign_request_id(request);

        LOCK_INFLIGHT();
        NetworkXioContextPtr ctx = atomic_get_ctx();
        r = (ctx.get()->*mem_fun)(request, std::forward<Args>(args)...);
        if (not r)
        {
            insert_inflight_request(id, request, std::move(ctx));
        }
    }
    else
    {
        r = (atomic_get_ctx().get()->*mem_fun)(request,
                                               std::forward<Args>(args)...);
    }
    return r;
}

int
NetworkHAContext::send_read_request(ovs_aio_request* request)
{
    return wrap_io(&NetworkXioContext::send_read_request,
                   request);
}

int
NetworkHAContext::send_write_request(ovs_aio_request* request)
{
    return wrap_io(&NetworkXioContext::send_write_request,
                   request);
}

int
NetworkHAContext::send_flush_request(ovs_aio_request* request)
{
    return wrap_io(&NetworkXioContext::send_flush_request,
                   request);
}

int
NetworkHAContext::stat_volume(struct stat *st)
{
    return atomic_get_ctx()->stat_volume(st);
}

ovs_buffer_t*
NetworkHAContext::allocate(size_t size)
{
    ovs_buffer_t *buf = new ovs_buffer_t;
    int r = xio_mempool_alloc(mpool.get(), size, &buf->mem);
    if (r < 0)
    {
        void *ptr = malloc(size);
        if (ptr == nullptr)
        {
            delete buf;
            return NULL;
        }
        else
        {
            buf->buf = ptr;
            buf->size = size;
            buf->from_mpool = false;
        }
    }
    else
    {
        buf->buf = buf->mem.addr;
        buf->size = buf->mem.length;
        buf->from_mpool = true;
    }
    return buf;
}

int
NetworkHAContext::deallocate(ovs_buffer_t *ptr)
{
    if (ptr->from_mpool)
    {
        xio_mempool_free(&ptr->mem);
    }
    else
    {
        free(ptr->buf);
    }
    delete ptr;
    return 0;
}

bool
NetworkHAContext::is_dtl_in_sync()
{
    return atomic_get_ctx()->is_dtl_in_sync();
}

int
NetworkHAContext::get_cluster_multiplier(const char *volume_name,
                                         uint32_t *cluster_multiplier)
{
    return atomic_get_ctx()->get_cluster_multiplier(volume_name,
                                                    cluster_multiplier);
}

int
NetworkHAContext::get_clone_namespace_map(const char *volume_name,
                                          CloneNamespaceMap& cn)
{
    return atomic_get_ctx()->get_clone_namespace_map(volume_name, cn);
}

int
NetworkHAContext::get_page(const char *volume_name,
                           const ClusterAddress ca,
                           ClusterLocationPage& cl)
{
    return atomic_get_ctx()->get_page(volume_name, ca, cl);
}

} //namespace libovsvolumedriver

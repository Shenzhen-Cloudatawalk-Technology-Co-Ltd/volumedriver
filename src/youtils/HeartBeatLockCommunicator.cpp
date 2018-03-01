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

#include "Catchers.h"
#include "HeartBeatLockCommunicator.h"
#include "System.h"

#include <boost/thread.hpp>

namespace youtils
{

namespace bc = boost::chrono;

unsigned
HeartBeatLockCommunicator::sleep_seconds()
{
    static const unsigned s = System::get_env_with_default("HEARTBEAT_LOCK_COMMUNICATOR_SLEEP_SECONDS",
                                                           1);
    return s;
}

uint64_t
HeartBeatLockCommunicator::max_update_retries()
{
    static const unsigned n = System::get_env_with_default("HEARTBEAT_LOCK_COMMUNICATOR_MAX_UPDATE_RETRIES",
                                                           100ULL);
    return n;
}

HeartBeatLockCommunicator::HeartBeatLockCommunicator(GlobalLockStorePtr lock_store,
                                                     const boost::posix_time::time_duration connection_timeout,
                                                     const boost::posix_time::time_duration interrupt_timeout)
    : lock_store_(lock_store)
    , lock_(connection_timeout,
            interrupt_timeout)
{}

bool
HeartBeatLockCommunicator::lock_exists()
{
    return lock_store_->exists();
}

void
HeartBeatLockCommunicator::freeLock()
{
    lock_.hasLock(false);
    try
    {
        overwriteLock();
    }
    CATCH_STD_ALL_LOG_IGNORE(name() << ": exception overwriting lock");
}

HeartBeatLock
HeartBeatLockCommunicator::getLock()
{
    LOG_INFO(name() << ": getting the lock and updating the tag");

    HeartBeatLock lock(lock_);
    std::tie(lock, tag_) = lock_store_->read();

    LOG_INFO(name() << ": new tag " << *tag_);
    return lock;
}

bool
HeartBeatLockCommunicator::overwriteLock()
{
    LOG_INFO(name() << ": overwriting the lock with tag " << *tag_);
    try
    {
        tag_ = lock_store_->write(lock_,
                                  tag_.get());

        LOG_INFO(name() << ": overwrote the lock");
        return true;
    }
    catch (GlobalLockStore::LockHasChanged& e)
    {
        LOG_INFO(name() << "failed to overwrite lock: " << e.what());
        return false;
    }
}

bool
HeartBeatLockCommunicator::putLock()
{
    LOG_INFO(name() << ": putting the lock");

    try
    {
        tag_ = lock_store_->write(lock_,
                                  nullptr);

        LOG_INFO(name() << ": put the lock");
        return true;
    }
    CATCH_STD_ALL_EWHAT({
            LOG_WARN(name() <<
                     ": exception while putting the lock: " << EWHAT);
            return false;
        });
}

bool
HeartBeatLockCommunicator::tryToAcquireLock()
{
    LOG_INFO(name() << ": trying to acquire the lock");
    try
    {
        HeartBeatLock l(getLock());
        if(not l.hasLock())
        {
            LOG_INFO(name() << ": lock is not owned, trying to get it");
            return overwriteLock();
        }
        else
        {
            boost::this_thread::sleep(l.get_timeout());
            LOG_INFO(name() <<
                     ": trying to steal the lock with tag " << *tag_);
            return overwriteLock();
        }
    }
    CATCH_STD_ALL_EWHAT({
            LOG_WARN(name() <<
                     ": exception while trying to acquire the lock " << EWHAT);
            return false;
        });
}

bool
HeartBeatLockCommunicator::refreshLock(MilliSeconds max_wait_time)
{
    LOG_INFO(name() << ": refreshing the lock");

    ++lock_;
    uint64_t count = 0;
    Clock::time_point start_of_update = Clock::now();

    while (count < max_update_retries())
    {
        LOG_INFO(name() <<
                 ": once more into the breach dear friends (retry " <<
                 count << ")");

        ++count;
        bool sleep = false;

        try
        {
            if (overwriteLock())
            {
                LOG_INFO(name() <<
                         ": lock was successfully refreshed");
                return true;
            }
        }
        CATCH_STD_ALL_EWHAT({
                LOG_ERROR(name() << ": exception while refreshing lock: " <<EWHAT);

                const Clock::duration update_duration = Clock::now() - start_of_update;
                if (update_duration + bc::seconds(sleep_seconds()) > max_wait_time)
                {
                    LOG_ERROR(name() << "max wait time " << max_wait_time <<
                              " would be exceeded, giving up");
                    throw;
                }
                else
                {
                    LOG_WARN(name() <<
                             ": refreshing the lock failed, we have " <<
                             bc::duration_cast<bc::seconds>(max_wait_time - update_duration) << " left");
                    sleep = true;
                }
            });

        if (sleep)
        {
            //boost::this_thread::sleep uses get_system_time() and might hang on system time reset
            ::sleep(sleep_seconds());
        }

        try
        {
            LOG_INFO(name() <<
                     ": reading the lock to see whether we're still owning it");
            const HeartBeatLock local_lock_(getLock());

            if (local_lock_.different_owner(lock_))
            {
                LOG_WARN(name() << ": lost the lock");
                return false;
            }
        }
        CATCH_STD_ALL_EWHAT({
                LOG_WARN(name() <<
                         ": exception getting the lock: " << EWHAT);
            });
    }

    LOG_ERROR(name() << ": ran out of retries");
    return false;
}

const std::string&
HeartBeatLockCommunicator::name() const
{
    return lock_store_->name();
}

}

// Local Variables: **
// mode: c++ **
// End: **

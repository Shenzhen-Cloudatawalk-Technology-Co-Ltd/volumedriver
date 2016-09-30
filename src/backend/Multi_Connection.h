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

#ifndef MULTI_CONNECTION_H
#define MULTI_CONNECTION_H

#include "BackendException.h"
#include "BackendConnectionInterface.h"
#include "MultiConfig.h"

#include <boost/filesystem.hpp>

#include <youtils/IOException.h>
#include <youtils/Logging.h>
#include <youtils/wall_timer.h>

namespace backend
{
namespace multi
{
namespace fs = boost::filesystem;

class Connection
    : public BackendConnectionInterface
{
public:
    typedef MultiConfig config_type;

    explicit Connection(const config_type& cfg);

    virtual ~Connection() = default;

    Connection(const Connection&) = delete;

    Connection&
    operator=(const Connection&) = delete;

    virtual void
    listNamespaces_(std::list<std::string>& objects) override final;

    virtual void
    listObjects_(const Namespace& nspace,
                 std::list<std::string>& objects) override final;

    virtual bool
    partial_read_(const Namespace&,
                  const PartialReads&,
                  InsistOnLatestVersion) override final;

    virtual bool
    namespaceExists_(const Namespace& nspace) override final;

    virtual void
    createNamespace_(const Namespace& nspace,
                     const NamespaceMustNotExist = NamespaceMustNotExist::T) override final;

    virtual void
    deleteNamespace_(const Namespace& nspace) override final;

    virtual bool
    healthy() const override final
    {
        return true;
    }

    virtual void
    read_(const Namespace& nspace,
          const fs::path& Destination,
          const std::string& Name,
          const InsistOnLatestVersion insist_on_latest) override final;

    virtual std::unique_ptr<youtils::UniqueObjectTag>
    get_tag_(const Namespace&,
             const std::string&) override final;

    virtual void
    write_(const Namespace&,
           const fs::path&,
           const std::string&,
           const OverwriteObject = OverwriteObject::F,
           const youtils::CheckSum* = nullptr,
           const boost::shared_ptr<Condition>& = nullptr) override final;

    virtual std::unique_ptr<youtils::UniqueObjectTag>
    write_tag_(const Namespace&,
               const boost::filesystem::path&,
               const std::string&,
               const youtils::UniqueObjectTag*,
               const OverwriteObject) override final;

    virtual std::unique_ptr<youtils::UniqueObjectTag>
    read_tag_(const Namespace&,
              const boost::filesystem::path&,
              const std::string&) override final;

    virtual bool
    objectExists_(const Namespace& nspace,
                  const std::string& name) override final;

    virtual void
    remove_(const Namespace&,
            const std::string&,
            const ObjectMayNotExist,
            const boost::shared_ptr<Condition>& = nullptr) override final;

    virtual uint64_t
    getSize_(const Namespace& nspace,
             const std::string& name) override final;

    virtual youtils::CheckSum
    getCheckSum_(const Namespace& nspace,
                 const std::string& name) override final;

private:
    DECLARE_LOGGER("MultiConnection");

    using ConnVector = std::vector<std::unique_ptr<BackendConnectionInterface>>;
    ConnVector backends_;
    ConnVector::iterator current_iterator_;
    youtils::wall_timer2 switch_back_timer_;
    const unsigned switch_back_seconds = 10 * 60;

    bool
    update_current_index(const ConnVector::iterator& start_iterator)
    {
        switch_back_timer_.restart();

        LOG_WARN("Trying to switch to a new backend");
        if(++current_iterator_ == backends_.end() )
        {
            current_iterator_ = backends_.begin();
        }

        return current_iterator_ == start_iterator;
    }

    bool
    maybe_switch_back_to_default()
    {
        if(current_iterator_ != backends_.begin())
        {
            if(switch_back_timer_.elapsed_in_seconds() > switch_back_seconds)
            {
                current_iterator_ = backends_.begin();
            }
        }
        return current_iterator_ != backends_.end();
    }

    template<typename Ret,
             typename... Args>
    Ret
    wrap_(Ret (BackendConnectionInterface::*fun)(Args...),
          Args...);
};

}

}



#endif // MULTI_CONNECTION_H

// Local Variables: **
// mode: c++ **
// End: **

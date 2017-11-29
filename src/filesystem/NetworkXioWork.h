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

#ifndef NETWORK_XIO_WORK_H_
#define NETWORK_XIO_WORK_H_

#include <functional>

#include <boost/thread/future.hpp>

namespace volumedriverfs
{

struct NetworkXioRequest;

struct Work
{
    typedef std::function<boost::future<NetworkXioRequest&>()> workitem_func_t;

    workitem_func_t func = nullptr;
    workitem_func_t func_ctrl = nullptr;
    std::function<void()> dispatch_ctrl_request = nullptr;
    bool is_ctrl = false;
};

} //namespace

#endif //NETWORK_XIO_WORK_H_

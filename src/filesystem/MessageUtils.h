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

#ifndef VFS_MESSAGE_UTILS_H_
#define VFS_MESSAGE_UTILS_H_

#include "Messages.pb.h"
#include "NodeId.h"
#include "Object.h"

#include <youtils/Logging.h>

#include <volumedriver/ClusterLocation.h>
#include <volumedriver/DtlInSync.h>
#include <volumedriver/Types.h>

namespace vfsprotocol
{

struct MessageUtils
{
    static PingMessage
    create_ping_message(const volumedriverfs::NodeId& sender_id);

    static ReadRequest
    create_read_request(const volumedriverfs::Object&,
                        const uint64_t size,
                        const uint64_t offset);

    static WriteRequest
    create_write_request(const volumedriverfs::Object&,
                         const uint64_t size,
                         const uint64_t offset);

    static WriteResponse
    create_write_response(const uint64_t size,
                          const volumedriver::DtlInSync);

    static SyncRequest
    create_sync_request(const volumedriverfs::Object&);

    static SyncResponse
    create_sync_response(const volumedriver::DtlInSync);

    static GetSizeRequest
    create_get_size_request(const volumedriverfs::Object&);

    static GetSizeResponse
    create_get_size_response(const uint64_t size);

    static GetClusterMultiplierRequest
    create_get_cluster_multiplier_request(const volumedriverfs::Object&);

    static GetClusterMultiplierResponse
    create_get_cluster_multiplier_response(const volumedriver::ClusterMultiplier);

    static GetCloneNamespaceMapRequest
    create_get_clone_namespace_map_request(const volumedriverfs::Object&);

    static GetCloneNamespaceMapResponse
    create_get_clone_namespace_map_response(const volumedriver::CloneNamespaceMap&);

    static GetPageRequest
    create_get_page_request(const volumedriverfs::Object&,
                            const volumedriver::ClusterAddress);

    static GetPageResponse
    create_get_page_response(const std::vector<volumedriver::ClusterLocation>&);

    static ResizeRequest
    create_resize_request(const volumedriverfs::Object&,
                          uint64_t newsize);

    static DeleteRequest
    create_delete_request(const volumedriverfs::Object&);

    static TransferRequest
    create_transfer_request(const volumedriverfs::Object&,
                            const volumedriverfs::NodeId& target_node_id,
                            const boost::chrono::milliseconds& sync_timeout_ms);

    static OpenRequest
    create_open_request(const volumedriverfs::Object&);
};

}

#endif //!VFS_MESSAGE_UTILS_H_

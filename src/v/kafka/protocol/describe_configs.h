/*
 * Copyright 2020 Vectorized, Inc.
 *
 * Use of this software is governed by the Business Source License
 * included in the file licenses/BSL.md
 *
 * As of the Change Date specified in that file, in accordance with
 * the Business Source License, use of this software will be governed
 * by the Apache License, Version 2.0
 */

#pragma once
#include "bytes/iobuf.h"
#include "kafka/errors.h"
#include "kafka/protocol/schemata/describe_configs_request.h"
#include "kafka/protocol/schemata/describe_configs_response.h"
#include "kafka/server/request_context.h"
#include "kafka/server/response.h"
#include "kafka/types.h"
#include "model/fundamental.h"
#include "model/metadata.h"
#include "model/timestamp.h"

#include <seastar/core/future.hh>

namespace kafka {

struct describe_configs_response;

class describe_configs_api final {
public:
    using response_type = describe_configs_response;

    static constexpr const char* name = "describe_configs";
    static constexpr api_key key = api_key(32);
};

struct describe_configs_request final {
    using api_type = describe_configs_api;

    describe_configs_request_data data;

    void encode(response_writer& writer, api_version version) {
        data.encode(writer, version);
    }

    void decode(request_reader& reader, api_version version) {
        data.decode(reader, version);
    }
};

inline std::ostream&
operator<<(std::ostream& os, const describe_configs_request& r) {
    return os << r.data;
}

struct describe_configs_response final {
    using api_type = describe_configs_api;

    describe_configs_response_data data;

    describe_configs_response()
      : data({.throttle_time_ms = std::chrono::milliseconds(0)}) {}

    void encode(const request_context& ctx, response& resp) {
        data.encode(resp.writer(), ctx.header().version);
    }

    void decode(iobuf buf, api_version version) {
        data.decode(std::move(buf), version);
    }
};

inline std::ostream&
operator<<(std::ostream& os, const describe_configs_response& r) {
    return os << r.data;
}

} // namespace kafka

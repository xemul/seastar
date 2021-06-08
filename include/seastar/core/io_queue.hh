/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License").  See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership.  You may not use this file except in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
/*
 * Copyright 2019 ScyllaDB
 */

#pragma once

#include <seastar/core/sstring.hh>
#include <seastar/core/fair_queue.hh>
#include <seastar/core/metrics_registration.hh>
#include <seastar/core/future.hh>
#include <seastar/core/internal/io_request.hh>
#include <seastar/util/staircase.hh>
#include <mutex>
#include <array>

namespace seastar {

class io_priority_class;

[[deprecated("Use io_priority_class.rename")]]
future<>
rename_priority_class(io_priority_class pc, sstring new_name);

class io_intent;

namespace internal {
class io_sink;
namespace linux_abi {

struct io_event;
struct iocb;

}
}

using shard_id = unsigned;

class io_priority_class;
class io_desc_read_write;
class queued_io_request;
class io_group;

using io_group_ptr = std::shared_ptr<io_group>;
class priority_class_data;

class io_queue {
private:
    std::vector<std::unique_ptr<priority_class_data>> _priority_classes;
    io_group_ptr _group;
    fair_queue _fq;
    internal::io_sink& _sink;

    priority_class_data& find_or_create_class(const io_priority_class& pc);

    unsigned _cancelled_requests = 0;
public:
    // We want to represent the fact that write requests are (maybe) more expensive
    // than read requests. To avoid dealing with floating point math we will scale one
    // read request to be counted by this amount.
    //
    // A write request that is 30% more expensive than a read will be accounted as
    // (read_request_base_count * 130) / 100.
    // It is also technically possible for reads to be the expensive ones, in which case
    // writes will have an integer value lower than read_request_base_count.
    static constexpr unsigned read_request_base_count = 128;
    static constexpr unsigned request_ticket_size_shift = 9;
    static constexpr unsigned minimal_request_size = 512;

    struct config {
        dev_t devid;
        unsigned capacity = std::numeric_limits<unsigned>::max();
        unsigned max_req_count = std::numeric_limits<int>::max();
        unsigned max_bytes_count = std::numeric_limits<int>::max();
        unsigned disk_req_write_multiplier = read_request_base_count;
        internal::staircase<unsigned, 3> disk_bytes_write_multiplier = internal::staircase<unsigned, 3>(read_request_base_count);
        internal::staircase<unsigned, 3> disk_bytes_read_multiplier = internal::staircase<unsigned, 3>(read_request_base_count);
        float disk_us_per_request = 0;
        float disk_us_per_byte = 0;
        size_t disk_read_saturation_length = std::numeric_limits<size_t>::max();
        size_t disk_write_saturation_length = std::numeric_limits<size_t>::max();
        sstring mountpoint = "undefined";
    };

    io_queue(io_group_ptr group, internal::io_sink& sink);
    ~io_queue();

    fair_queue_ticket request_fq_ticket_for_queue(internal::io_direction_and_length dnl) const noexcept;

    future<size_t>
    queue_request(const io_priority_class& pc, size_t len, internal::io_request req, io_intent* intent) noexcept;
    void submit_request(io_desc_read_write* desc, internal::io_request req) noexcept;
    void cancel_request(queued_io_request& req) noexcept;
    void complete_cancelled_request(queued_io_request& req) noexcept;

    [[deprecated("modern I/O queues should use a property file")]] size_t capacity() const;

    [[deprecated("I/O queue users should not track individual requests, but resources (weight, size) passing through the queue")]]
    size_t queued_requests() const {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
        return _fq.waiters() - _cancelled_requests;
#pragma GCC diagnostic pop
    }

    // How many requests are sent to disk but not yet returned.
    [[deprecated("I/O queue users should not track individual requests, but resources (weight, size) passing through the queue")]]
    size_t requests_currently_executing() const {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
        return _fq.requests_currently_executing();
#pragma GCC diagnostic pop
    }

    void notify_request_finished(fair_queue_ticket x_ticket) noexcept;

    // Dispatch requests that are pending in the I/O queue
    void poll_io_queue();

    std::chrono::steady_clock::time_point next_pending_aio() const noexcept {
        return _fq.next_pending_aio();
    }

    sstring mountpoint() const;
    dev_t dev_id() const noexcept;

    future<> update_shares_for_class(io_priority_class pc, size_t new_shares);
    void rename_priority_class(io_priority_class pc, sstring new_name);

    struct request_limits {
        size_t max_read;
        size_t max_write;
    };

    request_limits get_request_limits() const noexcept;

private:
    static fair_queue::config make_fair_queue_config(config cfg);

public:
    const config& get_config() const noexcept;
};

class io_group {
public:
    explicit io_group(io_queue::config io_cfg) noexcept;

    fair_queue_ticket request_fq_ticket(internal::io_direction_and_length dnl) const noexcept;

private:
    friend class io_queue;
    fair_group _fg;
    const io_queue::config _config;

    static fair_group::config make_fair_group_config(io_queue::config qcfg) noexcept;
    fair_queue_ticket make_ticket(unsigned weight, size_t sz, size_t len) const noexcept;
};

inline const io_queue::config& io_queue::get_config() const noexcept {
    return _group->_config;
}

inline size_t io_queue::capacity() const {
    return get_config().capacity;
}

inline sstring io_queue::mountpoint() const {
    return get_config().mountpoint;
}

inline dev_t io_queue::dev_id() const noexcept {
    return get_config().devid;
}

}

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

#include <boost/intrusive/parent_from_member.hpp>
#include <seastar/core/fair_queue.hh>
#include <seastar/core/future.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/circular_buffer.hh>
#include <seastar/util/noncopyable_function.hh>
#include <seastar/core/reactor.hh>
#include <queue>
#include <chrono>
#include <unordered_set>
#include <cmath>

#include "fmt/format.h"
#include "fmt/ostream.h"

namespace seastar {

static_assert(sizeof(fair_queue_ticket) == sizeof(uint64_t), "unexpected fair_queue_ticket size");

template <typename T>
SEASTAR_CONCEPT( requires fair_queue_schedulable<T> )
fair_group_impl<T>::fair_group_impl(config cfg) noexcept
        : _capacity_tail(fair_group_rover(0, 0))
        , _capacity_head(fair_group_rover(cfg.max_req_count, cfg.max_bytes_count))
        , _maximum_capacity(cfg.max_req_count, cfg.max_bytes_count)
{
    assert(!_capacity_tail.load(std::memory_order_relaxed)
                .maybe_ahead_of(_capacity_head.load(std::memory_order_relaxed)));
    seastar_logger.debug("Created fair group, capacity {}:{}", cfg.max_req_count, cfg.max_bytes_count);
}

template <typename T>
SEASTAR_CONCEPT( requires fair_queue_schedulable<T> )
fair_group_rover fair_group_impl<T>::grab_capacity(fair_queue_ticket cap) noexcept {
    fair_group_rover cur = _capacity_tail.load(std::memory_order_relaxed);
    while (!_capacity_tail.compare_exchange_weak(cur, cur + cap)) ;
    return cur;
}

template <typename T>
SEASTAR_CONCEPT( requires fair_queue_schedulable<T> )
void fair_group_impl<T>::release_capacity(fair_queue_ticket cap) noexcept {
    fair_group_rover cur = _capacity_head.load(std::memory_order_relaxed);
    while (!_capacity_head.compare_exchange_weak(cur, cur + cap)) ;
}

// Priority class, to be used with a given fair_queue
template <typename T>
SEASTAR_CONCEPT( requires fair_queue_schedulable<T> )
class fair_queue_impl<T>::priority_class_data {
    using accumulator_t = double;
    friend class fair_queue_impl;
    uint32_t _shares = 0;
    accumulator_t _accumulated = 0;
    bi::slist<T, bi::constant_time_size<false>, bi::cache_last<true>> _queue;
    bool _queued = false;

public:
    explicit priority_class_data(uint32_t shares) noexcept : _shares(std::max(shares, 1u)) {}

    void update_shares(uint32_t shares) noexcept {
        _shares = (std::max(shares, 1u));
    }
};

template <typename T>
SEASTAR_CONCEPT( requires fair_queue_schedulable<T> )
bool fair_queue_impl<T>::class_compare::operator() (const priority_class_ptr& lhs, const priority_class_ptr & rhs) const noexcept {
    return lhs->_accumulated > rhs->_accumulated;
}

template <typename T>
SEASTAR_CONCEPT( requires fair_queue_schedulable<T> )
fair_queue_impl<T>::fair_queue_impl(fair_group_impl<T>& group, config cfg)
    : _config(std::move(cfg))
    , _group(group)
    , _base(std::chrono::steady_clock::now())
{
    seastar_logger.debug("Created fair queue, ticket pace {}:{}", cfg.ticket_weight_pace, cfg.ticket_size_pace);
}

template <typename T>
SEASTAR_CONCEPT( requires fair_queue_schedulable<T> )
fair_queue_impl<T>::fair_queue_impl(fair_queue_impl&& other)
    : _config(std::move(other._config))
    , _group(other._group)
    , _base(other._base)
    , _handles(std::move(other._handles))
    , _priority_classes(std::move(other._priority_classes))
{
}

template <typename T>
SEASTAR_CONCEPT( requires fair_queue_schedulable<T> )
fair_queue_impl<T>::~fair_queue_impl() {
    for (const auto& fq : _priority_classes) {
        assert(!fq);
    }
}

template <typename T>
SEASTAR_CONCEPT( requires fair_queue_schedulable<T> )
void fair_queue_impl<T>::push_priority_class(priority_class_data& pc) {
    if (!pc._queued) {
        _handles.push(&pc);
        pc._queued = true;
    }
}

template <typename T>
SEASTAR_CONCEPT( requires fair_queue_schedulable<T> )
void fair_queue_impl<T>::pop_priority_class(priority_class_data& pc) {
    assert(pc._queued);
    pc._queued = false;
    _handles.pop();
}

template <typename T>
SEASTAR_CONCEPT( requires fair_queue_schedulable<T> )
void fair_queue_impl<T>::normalize_stats() {
    _base = std::chrono::steady_clock::now() - _config.tau;
    for (auto& pc: _priority_classes) {
        if (pc) {
            pc->_accumulated *= std::numeric_limits<typename priority_class_data::accumulator_t>::min();
        }
    }
}

template <typename T>
SEASTAR_CONCEPT( requires fair_queue_schedulable<T> )
bool fair_queue_impl<T>::grab_pending_capacity(fair_queue_ticket cap) noexcept {
    fair_group_rover pending_head = _pending->orig_tail + cap;
    if (pending_head.maybe_ahead_of(_group.head())) {
        return false;
    }

    if (cap == _pending->cap) {
        _pending.reset();
    } else {
        /*
         * This branch is called when the fair queue decides to
         * submit not the same request that entered it into the
         * pending state and this new request crawls through the
         * expected head value.
         */
        _group.grab_capacity(cap);
        _pending->orig_tail += cap;
    }

    return true;
}

template <typename T>
SEASTAR_CONCEPT( requires fair_queue_schedulable<T> )
bool fair_queue_impl<T>::grab_capacity(fair_queue_ticket cap) noexcept {
    if (_pending) {
        return grab_pending_capacity(cap);
    }

    fair_group_rover orig_tail = _group.grab_capacity(cap);
    if ((orig_tail + cap).maybe_ahead_of(_group.head())) {
        _pending.emplace(orig_tail, cap);
        return false;
    }

    return true;
}

template <typename T>
SEASTAR_CONCEPT( requires fair_queue_schedulable<T> )
void fair_queue_impl<T>::register_priority_class(class_id id, uint32_t shares) {
    if (id >= _priority_classes.size()) {
        _priority_classes.resize(id + 1);
    } else {
        assert(!_priority_classes[id]);
    }

    _priority_classes[id] = std::make_unique<priority_class_data>(shares);
}

template <typename T>
SEASTAR_CONCEPT( requires fair_queue_schedulable<T> )
void fair_queue_impl<T>::unregister_priority_class(class_id id) {
    auto& pclass = _priority_classes[id];
    assert(pclass && pclass->_queue.empty());
    pclass.reset();
}

template <typename T>
SEASTAR_CONCEPT( requires fair_queue_schedulable<T> )
void fair_queue_impl<T>::update_shares_for_class(class_id id, uint32_t shares) {
    assert(id < _priority_classes.size());
    auto& pc = _priority_classes[id];
    assert(pc);
    pc->update_shares(shares);
}

template <typename T>
SEASTAR_CONCEPT( requires fair_queue_schedulable<T> )
void fair_queue_impl<T>::queue(class_id id, T& ent) {
    priority_class_data& pc = *_priority_classes[id];
    // We need to return a future in this function on which the caller can wait.
    // Since we don't know which queue we will use to execute the next request - if ours or
    // someone else's, we need a separate promise at this point.
    push_priority_class(pc);
    pc._queue.push_back(ent);
}

template <typename T>
SEASTAR_CONCEPT( requires fair_queue_schedulable<T> )
void fair_queue_impl<T>::notify_request_finished(fair_queue_ticket desc) noexcept {
    _group.release_capacity(desc);
}

template <typename T>
SEASTAR_CONCEPT( requires fair_queue_schedulable<T> )
void fair_queue_impl<T>::dispatch_requests() {
    while (!_handles.empty()) {
        priority_class_data& h = *_handles.top();
        if (h._queue.empty()) {
            pop_priority_class(h);
            continue;
        }

        T& req = h._queue.front();
        if (!grab_capacity(req.ticket())) {
            break;
        }

        pop_priority_class(h);
        h._queue.pop_front();

        auto delta = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - _base);
        auto req_cost  = req.ticket().normalize(_group.maximum_capacity()) / h._shares;
        auto cost  = exp(typename priority_class_data::accumulator_t(1.0f/_config.tau.count() * delta.count())) * req_cost;
        typename priority_class_data::accumulator_t next_accumulated = h._accumulated + cost;
        while (std::isinf(next_accumulated)) {
            normalize_stats();
            // If we have renormalized, our time base will have changed. This should happen very infrequently
            delta = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - _base);
            cost  = exp(typename priority_class_data::accumulator_t(1.0f/_config.tau.count() * delta.count())) * req_cost;
            next_accumulated = h._accumulated + cost;
        }
        h._accumulated = next_accumulated;

        if (!h._queue.empty()) {
            push_priority_class(h);
        }

        req.dispatch();
    }
}

template <typename T>
SEASTAR_CONCEPT( requires fair_queue_schedulable<T> )
auto fair_queue_impl<T>::next_pending_aio() const noexcept -> clock_type {
    if (_pending) {
        /*
         * We expect the disk to release the ticket within some time,
         * but it's ... OK if it doesn't -- the pending wait still
         * needs the head rover value to be ahead of the needed value.
         *
         * It may happen that the capacity gets released before we think
         * it will, in this case we will wait for the full value again,
         * which's sub-optimal. The expectation is that we think disk
         * works faster, than it really does.
         */
        fair_group_rover pending_head = _pending->orig_tail + _pending->cap;
        fair_queue_ticket over = pending_head.maybe_ahead_of(_group.head());
        return std::chrono::steady_clock::now() + duration(over);
    }

    return std::chrono::steady_clock::time_point::max();
}

}

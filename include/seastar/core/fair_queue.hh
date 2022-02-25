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
 * Copyright (C) 2016 ScyllaDB
 */
#pragma once

#include <boost/intrusive/slist.hpp>
#include <seastar/core/sstring.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/circular_buffer.hh>
#include <seastar/core/metrics_registration.hh>
#include <functional>
#include <atomic>
#include <queue>
#include <chrono>
#include <unordered_set>
#include <optional>
#include <cmath>

namespace bi = boost::intrusive;

namespace seastar {

/// \brief describes a request that passes through the \ref fair_queue.
///
/// A ticket is specified by a \c weight and a \c size. For example, one can specify a request of \c weight
/// 1 and \c size 16kB. If the \ref fair_queue accepts one such request per second, it will sustain 1 IOPS
/// at 16kB/s bandwidth.
///
/// \related fair_queue
class fair_queue_ticket {
    uint32_t _weight = 0; ///< the total weight of these requests for capacity purposes (IOPS).
    uint32_t _size = 0;        ///< the total effective size of these requests
public:
    /// Constructs a fair_queue_ticket with a given \c weight and a given \c size
    ///
    /// \param weight the weight of the request
    /// \param size the size of the request
    fair_queue_ticket(uint32_t weight, uint32_t size) noexcept;
    fair_queue_ticket() noexcept {}
    fair_queue_ticket operator+(fair_queue_ticket desc) const noexcept;
    fair_queue_ticket operator-(fair_queue_ticket desc) const noexcept;
    /// Increase the quantity represented in this ticket by the amount represented by \c desc
    /// \param desc another \ref fair_queue_ticket whose \c weight \c and size will be added to this one
    fair_queue_ticket& operator+=(fair_queue_ticket desc) noexcept;
    /// Decreases the quantity represented in this ticket by the amount represented by \c desc
    /// \param desc another \ref fair_queue_ticket whose \c weight \c and size will be decremented from this one
    fair_queue_ticket& operator-=(fair_queue_ticket desc) noexcept;
    /// Checks if the tickets fully equals to another one
    /// \param desc another \ref fair_queue_ticket to compare with
    bool operator==(const fair_queue_ticket& desc) const noexcept;

    /// \returns true if the fair_queue_ticket represents a non-zero quantity.
    ///
    /// For a fair_queue ticket to be non-zero, at least one of its represented quantities need to
    /// be non-zero
    explicit operator bool() const noexcept;
    bool is_non_zero() const noexcept;

    friend std::ostream& operator<<(std::ostream& os, fair_queue_ticket t);

    /// \returns the normalized value of this \ref fair_queue_ticket along a base axis
    ///
    /// The normalization function itself is an implementation detail, but one can expect either weight or
    /// size to have more or less relative importance depending on which of the dimensions in the
    /// denominator is relatively higher. For example, given this request a, and two other requests
    /// b and c, such that that c has the same \c weight but a higher \c size than b, one can expect
    /// the \c size component of this request to play a larger role.
    ///
    /// It is legal for the numerator to have one of the quantities set to zero, in which case only
    /// the other quantity is taken into consideration.
    ///
    /// It is however not legal for the axis to have any quantity set to zero.
    /// \param axis another \ref fair_queue_ticket to be used as a a base vector against which to normalize this fair_queue_ticket.
    float normalize(fair_queue_ticket axis) const noexcept;

    /*
     * For both dimentions checks if the first rover is ahead of the
     * second and returns the difference. If behind returns zero.
     */
    friend fair_queue_ticket wrapping_difference(const fair_queue_ticket& a, const fair_queue_ticket& b) noexcept;
};

/// \addtogroup io-module
/// @{

class fair_queue_entry {
    friend class fair_queue;

    fair_queue_ticket _ticket;
    bi::slist_member_hook<> _hook;

public:
    fair_queue_entry(fair_queue_ticket t) noexcept
        : _ticket(std::move(t)) {}
    using container_list_t = bi::slist<fair_queue_entry,
            bi::constant_time_size<false>,
            bi::cache_last<true>,
            bi::member_hook<fair_queue_entry, bi::slist_member_hook<>, &fair_queue_entry::_hook>>;

    fair_queue_ticket ticket() const noexcept { return _ticket; }
};

/// \brief Group of queues class
///
/// This is a fair group. It's attached by one or mode fair queues. On machines having the
/// big* amount of shards, queues use the group to borrow/lend the needed capacity for
/// requests dispatching.
///
/// * Big means that when all shards sumbit requests alltogether the disk is unable to
/// dispatch them efficiently. The inability can be of two kinds -- either disk cannot
/// cope with the number of arriving requests, or the total size of the data withing
/// the given time frame exceeds the disk throughput.
class fair_group {
public:
    using capacity_t = uint64_t;
    using clock_type = std::chrono::steady_clock;

    /*
     * tldr; The math
     *
     *    Bw, Br -- write/read bandwidth (bytes per second)
     *    Ow, Or -- write/read iops (ops per second)
     *
     *    xx_max -- their maximum values (configured)
     *
     * Throttling formula:
     *
     *    Bw/Bw_max + Br/Br_max + Ow/Ow_max + Or/Or_max <= K
     *
     * where K is the scalar value <= 1.0 (also configured)
     *
     * Bandwidth is bytes time derivatite, iops is ops time derivative, i.e.
     * Bx = d(bx)/dt, Ox = d(ox)/dt. Then the formula turns into
     *
     *   d(bw/Bw_max + br/Br_max + ow/Ow_max + or/Or_max)/dt <= K
     *
     * Fair queue tickets are {w, s} weight-size pairs that are
     *
     *   s = read_base_count * br, for reads
     *       Br_max/Bw_max * read_base_count * bw, for writes
     *
     *   w = read_base_count, for reads
     *       Or_max/Ow_max * read_base_count, for writes
     *
     * Thus the formula turns into
     *
     *   d(sum(w/W + s/S))/dr <= K
     *
     * where {w, s} is the ticket value if a request and sum summarizes the
     * ticket values from all the requests seen so far, {W, S} is the ticket
     * value that corresonds to a virtual summary of Or_max requests of
     * Br_max size total.
     */

private:
    using fair_group_atomic_rover = std::atomic<capacity_t>;
    static_assert(fair_group_atomic_rover::is_always_lock_free);

    /*
     * The dF/dt <= K limitation is managed by the modified token bucket
     * algo where tokens are ticket.normalize(cost_capacity), the refill
     * rate is K.
     *
     * The token bucket algo must have the limit on the number of tokens
     * accumulated. Here it's configured so that it accumulates for the
     * latency_goal duration.
     *
     * The replenish threshold is the minimal number of tokens to put back.
     * It's reserved for future use to reduce the load on the replenish
     * timestamp.
     *
     * The timestamp, in turn, is the time when the bucket was replenished
     * last. Every time a shard tries to get tokens from bucket it first
     * tries to convert the time that had passed since this timestamp
     * into more tokens in the bucket.
     */

    const fair_queue_ticket _cost_capacity;
    const capacity_t _replenish_rate;
    const capacity_t _replenish_limit;
    const capacity_t _replenish_threshold;
    std::atomic<clock_type::time_point> _replenished;

    /*
     * The token bucket is implemented as a pair of wrapping monotonic
     * counters (called rovers) one chasing the other. Getting a token
     * from the bucket is increasing the tail, replenishing a token back
     * is increasing the head. If increased tail overruns the head then
     * the bucket is empty and we have to wait. The shard that grabs tail
     * earlier will be "woken up" earlier, so they form a queue.
     *
     * The top rover is needed to implement two buckets actually. The
     * tokens are not just replenished by timer. They are replenished by
     * timer from the second bucket. And the second bucket only get a
     * token in it after the request that grabbed it from the first bucket
     * completes and returns it back.
     */

    fair_group_atomic_rover _capacity_tail;
    fair_group_atomic_rover _capacity_head;
    fair_group_atomic_rover _capacity_ceil;

    capacity_t fetch_add(fair_group_atomic_rover& rover, capacity_t cap) noexcept;

    template <typename Rep, typename Period>
    static auto rate_cast(const std::chrono::duration<Rep, Period> delta) noexcept {
        return std::chrono::duration_cast<rate_resolution>(delta);
    }

    template <typename Rep, typename Period>
    capacity_t accumulated_capacity(const std::chrono::duration<Rep, Period> delta) const noexcept {
       auto delta_at_rate = rate_cast(delta);
       return std::round(_replenish_rate * delta_at_rate.count());
    }

public:

    /*
     * The normalization results in a float of the 2^-30 seconds order of
     * magnitude. Not to invent float point atomic arithmetics, the result
     * is converted to an integer by multiplying by a factor that's large
     * enough to turn these values into a non-zero integer.
     *
     * Also, the rates in bytes/sec when adjusted by io-queue according to
     * multipliers become too large to be stored in 32-bit ticket value.
     * Thus the rate resolution is applied. The rate_resolution is the
     * time period for which the speeds from F (in above formula) are taken.
     */

    using rate_resolution = std::chrono::duration<double, std::milli>;
    static constexpr float fixed_point_factor = float(1 << 24);

    // Convert internal capacity value back into the real token
    static double capacity_tokens(capacity_t cap) noexcept {
        return (double)cap / fixed_point_factor / rate_cast(std::chrono::seconds(1)).count();
    }

    // Estimated time to process the given amount of capacity
    // (peer of accumulated_capacity() helper)
    rate_resolution capacity_duration(capacity_t cap) const noexcept {
        return rate_resolution(cap / _replenish_rate);
    }

    struct config {
        sstring label = "";
        unsigned min_weight = 0;
        unsigned min_size = 0;
        unsigned long weight_rate;
        unsigned long size_rate;
        float rate_factor = 1.0;
        std::chrono::duration<double> rate_limit_duration = std::chrono::milliseconds(1);
    };

    explicit fair_group(config cfg);
    fair_group(fair_group&&) = delete;

    fair_queue_ticket cost_capacity() const noexcept { return _cost_capacity; }
    capacity_t maximum_capacity() const noexcept { return _replenish_limit; }
    capacity_t grab_capacity(capacity_t cap) noexcept;
    clock_type::time_point replenished_ts() const noexcept { return _replenished; }
    void release_capacity(capacity_t cap) noexcept;
    void replenish_capacity(clock_type::time_point now) noexcept;
    void maybe_replenish_capacity(clock_type::time_point& local_ts) noexcept;

    capacity_t capacity_deficiency(capacity_t from) const noexcept;
    capacity_t ticket_capacity(fair_queue_ticket ticket) const noexcept;
};

/// \brief Fair queuing class
///
/// This is a fair queue, allowing multiple request producers to queue requests
/// that will then be served proportionally to their classes' shares.
///
/// To each request, a weight can also be associated. A request of weight 1 will consume
/// 1 share. Higher weights for a request will consume a proportionally higher amount of
/// shares.
///
/// The user of this interface is expected to register multiple `priority_class_data`
/// objects, which will each have a shares attribute.
///
/// Internally, each priority class may keep a separate queue of requests.
/// Requests pertaining to a class can go through even if they are over its
/// share limit, provided that the other classes have empty queues.
///
/// When the classes that lag behind start seeing requests, the fair queue will serve
/// them first, until balance is restored. This balancing is expected to happen within
/// a certain time window that obeys an exponential decay.
class fair_queue {
public:
    /// \brief Fair Queue configuration structure.
    ///
    /// \sets the operation parameters of a \ref fair_queue
    /// \related fair_queue
    struct config {
        sstring label = "";
        std::chrono::microseconds tau = std::chrono::milliseconds(5);
    };

    using class_id = unsigned int;
    class priority_class_data;
    using capacity_t = fair_group::capacity_t;
    using signed_capacity_t = std::make_signed<capacity_t>::type;

private:
    using clock_type = std::chrono::steady_clock;
    using priority_class_ptr = priority_class_data*;
    struct class_compare {
        bool operator() (const priority_class_ptr& lhs, const priority_class_ptr & rhs) const noexcept;
    };

    config _config;
    fair_group& _group;
    clock_type::time_point _group_replenish;
    fair_queue_ticket _resources_executing;
    fair_queue_ticket _resources_queued;
    unsigned _requests_executing = 0;
    unsigned _requests_queued = 0;
    using prioq = std::priority_queue<priority_class_ptr, std::vector<priority_class_ptr>, class_compare>;
    prioq _handles;
    std::vector<std::unique_ptr<priority_class_data>> _priority_classes;
    capacity_t _last_accumulated = 0;

    /*
     * When the shared capacity os over the local queue delays
     * further dispatching untill better times
     *
     * \head  -- the value group head rover is expected to cross
     * \cap   -- the capacity that's accounted on the group
     *
     * The last field is needed to "rearm" the wait in case
     * queue decides that it wants to dispatch another capacity
     * in the middle of the waiting
     */
    struct pending {
        capacity_t head;
        capacity_t cap;

        pending(capacity_t t, capacity_t c) noexcept : head(t), cap(c) {}
    };

    std::optional<pending> _pending;

    void push_priority_class(priority_class_data& pc);
    void push_priority_class_from_idle(priority_class_data& pc);
    void pop_priority_class(priority_class_data& pc);

    enum class grab_result { grabbed, pending };
    grab_result grab_capacity(const fair_queue_entry& ent) noexcept;
    grab_result grab_pending_capacity(const fair_queue_entry& ent) noexcept;
public:
    /// Constructs a fair queue with configuration parameters \c cfg.
    ///
    /// \param cfg an instance of the class \ref config
    explicit fair_queue(fair_group& shared, config cfg);
    fair_queue(fair_queue&&);
    ~fair_queue();

    sstring label() const noexcept { return _config.label; }

    /// Registers a priority class against this fair queue.
    ///
    /// \param shares how many shares to create this class with
    void register_priority_class(class_id c, uint32_t shares);

    /// Unregister a priority class.
    ///
    /// It is illegal to unregister a priority class that still have pending requests.
    void unregister_priority_class(class_id c);

    void update_shares_for_class(class_id c, uint32_t new_shares);

    /// \return how many waiters are currently queued for all classes.
    [[deprecated("fair_queue users should not track individual requests, but resources (weight, size) passing through the queue")]]
    size_t waiters() const;

    /// \return the number of requests currently executing
    [[deprecated("fair_queue users should not track individual requests, but resources (weight, size) passing through the queue")]]
    size_t requests_currently_executing() const;

    /// \return how much resources (weight, size) are currently queued for all classes.
    fair_queue_ticket resources_currently_waiting() const;

    /// \return the amount of resources (weight, size) currently executing
    fair_queue_ticket resources_currently_executing() const;

    /// Queue the entry \c ent through this class' \ref fair_queue
    ///
    /// The user of this interface is supposed to call \ref notify_requests_finished when the
    /// request finishes executing - regardless of success or failure.
    void queue(class_id c, fair_queue_entry& ent);

    /// Notifies that ont request finished
    /// \param desc an instance of \c fair_queue_ticket structure describing the request that just finished.
    void notify_request_finished(fair_queue_ticket desc) noexcept;
    void notify_request_cancelled(fair_queue_entry& ent) noexcept;

    /// Try to execute new requests if there is capacity left in the queue.
    void dispatch_requests(std::function<void(fair_queue_entry&)> cb);

    clock_type::time_point next_pending_aio() const noexcept;

    std::vector<seastar::metrics::impl::metric_definition_impl> metrics(class_id c);
};
/// @}

}

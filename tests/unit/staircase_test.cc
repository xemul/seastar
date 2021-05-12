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
 * Copyright 2021 ScyllaDB
 */

#include <seastar/testing/test_case.hh>
#include <seastar/testing/thread_test_case.hh>
#include <seastar/core/do_with.hh>
#include <seastar/util/staircase.hh>
#include <fmt/core.h>

using namespace seastar;

SEASTAR_TEST_CASE(deferred_stop_test) {
    constexpr size_t min = std::numeric_limits<size_t>::min();
    constexpr size_t max = std::numeric_limits<size_t>::max();

    util::staircase<int, 5> stairs(1);
    auto check_interval = [&stairs] (size_t from, size_t to, int val) {
        BOOST_REQUIRE(stairs.at(from) == val);
        BOOST_REQUIRE(stairs.at(to) == val);
    };

    check_interval(min, max, 1);

    stairs.add_step(100, 2);
    check_interval(min, 100, 2);
    check_interval(101, max, 1);

    stairs.add_step(200, 3);
    check_interval(min, 100, 2);
    check_interval(101, 200, 3);
    check_interval(201, max, 1);

    stairs.add_step(150, 4);
    check_interval(min, 100, 2);
    check_interval(101, 150, 4);
    check_interval(151, 200, 3);
    check_interval(201, max, 1);

    stairs.add_step(50, 5);
    check_interval(min, 50, 5);
    check_interval(51, 100, 2);
    check_interval(101, 150, 4);
    check_interval(151, 200, 3);
    check_interval(201, max, 1);

    stairs.add_step(250, 6);
    check_interval(min, 50, 5);
    check_interval(51, 100, 2);
    check_interval(101, 150, 4);
    check_interval(151, 200, 3);
    check_interval(201, 250, 6);
    check_interval(251, max, 1);

    fmt::print("{}\n", stairs);

    return make_ready_future<>();
}

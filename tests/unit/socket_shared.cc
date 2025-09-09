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
 * Copyright (C) 2025 ScyllaDB
 */

#undef SEASTAR_TESTING_MAIN

#include <fmt/core.h>
#include <seastar/testing/test_case.hh>
#include <seastar/net/api.hh>
#include <seastar/core/sstring.hh>
#include <seastar/util/closeable.hh>
#include "socket_shared.hh"

using namespace seastar;

namespace testing {

using recv_early = bool_class<struct recv_early_tag>;
using close_early = bool_class<struct close_early_tag>;

void do_socket_shutdown_sanity_test(
        connected_socket s, recv_early sre, close_early sce,
        connected_socket c, recv_early cre, close_early cce
) {
    auto out_s = s.output();
    auto close_out_s = deferred_close(out_s);
    auto in_s = s.input();
    auto close_in_s = deferred_close(in_s);
    auto out_c = c.output();
    auto close_out_c = deferred_close(out_c);
    auto in_c = c.input();
    auto close_in_c = deferred_close(in_c);

    future<temporary_buffer<char>> srcf = make_ready_future<temporary_buffer<char>>();
    future<temporary_buffer<char>> crcf = make_ready_future<temporary_buffer<char>>();

    if (sre) {
        fmt::print("server recv (in bg)\n");
        srcf = in_s.read_exactly(5);
    }

    fmt::print("client send\n");
    out_c.write("hello").get();
    out_c.flush().get();

    if (cce) {
        fmt::print("client closes write\n");
        close_out_c.close_now();
    }

    if (cre) {
        fmt::print("client recv (in bg)\n");
        crcf = in_c.read_exactly(5);
    }

    if (!sre) {
        fmt::print("server recv\n");
        srcf = in_s.read_exactly(5);
    }

    auto rs = std::move(srcf).get();
    fmt::print("server recvd: [{}]\n", internal::to_sstring<sstring>(rs));
    BOOST_REQUIRE_EQUAL(internal::to_sstring<sstring>(rs), "hello");
    fmt::print("server send\n");
    out_s.write(rs.get(), rs.size()).get();
    out_s.flush().get();

    if (sce) {
        fmt::print("server closes write\n");
        close_out_s.close_now();
    }

    if (!cre) {
        fmt::print("client recv\n");
        crcf = in_c.read_exactly(5);
    }

    auto rc = std::move(crcf).get();
    fmt::print("client recvd: [{}]\n", internal::to_sstring<sstring>(rc));
    BOOST_REQUIRE_EQUAL(internal::to_sstring<sstring>(rc), "hello");
}

void socket_shutdown_sanity_test(std::function<std::pair<connected_socket, connected_socket>()> socketpair) {
    for (recv_early cre : { recv_early::yes, recv_early::no }) {
        for (close_early cce : { close_early::yes, close_early::no }) {
            for (recv_early sre : { recv_early::yes, recv_early::no }) {
                for (close_early sce : { close_early::yes, close_early::no }) {
                    fmt::print("=== Server: early recv: {} early close: {} / Client: early recv: {} early close: {}\n", sre, sce, cre, cce);
                    auto p = socketpair();
                    do_socket_shutdown_sanity_test(std::move(p.first), sre, sce, std::move(p.second), cre, cce);
                }
            }
        }
    }
}

}

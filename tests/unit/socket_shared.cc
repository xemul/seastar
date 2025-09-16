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
#include <seastar/core/when_all.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/sleep.hh>
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

void socket_read_shutdown_sanity_test(std::function<std::pair<connected_socket, connected_socket>()> socketpair) {
    {
        fmt::print("Test shutdown_input wakeup read\n");
        auto p = socketpair();
        auto in = p.first.input();

        auto in_f = in.read();
        BOOST_CHECK(!in_f.available());
        p.first.shutdown_input();

        auto start = std::chrono::steady_clock::now();
        auto b = in_f.get();
        auto delay = std::chrono::duration_cast<std::chrono::duration<double>>(std::chrono::steady_clock::now() - start);
        BOOST_CHECK_EQUAL(b.size(), 0);
        fmt::print("Woke up in {} seconds\n", delay.count());
        BOOST_CHECK_LT(delay.count(), 1.0);
        b = in.read().get();
        BOOST_CHECK_EQUAL(b.size(), 0);
    }
    {
        fmt::print("Test shutdown_input with data\n");
        auto p = socketpair();
        auto in = p.first.input();
        auto out = p.second.output();

        out.write("hello").get();
        out.flush().get();

        auto b = in.read_exactly(1).get();
        BOOST_CHECK_EQUAL(internal::to_sstring<sstring>(b), "h");
        p.first.shutdown_input();

        b = in.read().get();
        BOOST_CHECK_EQUAL(internal::to_sstring<sstring>(b), "ello");
        b = in.read().get();
        BOOST_CHECK_EQUAL(b.size(), 0);
    }
}

void socket_close_with_unread_buffers_test(std::function<std::pair<connected_socket, connected_socket>()> socketpair) {
    auto p = socketpair();
    auto c = seastar::async([c = std::move(p.first)] mutable {
        auto in = c.input();
        auto b = in.read_exactly(1).get();
        in.close().get();
        c.shutdown_output();
    });

    auto s = seastar::async([s = std::move(p.second)] mutable {
        auto out = s.output();
        size_t bytes_sent = 0;
        auto buf = temporary_buffer<char>(1024);
        std::memset(buf.get_write(), '\0', buf.size());
        auto start = std::chrono::steady_clock::now();
        while (true) {
            try {
                out.write(buf.get(), buf.size()).get();
                out.flush().get();
                bytes_sent += buf.size();
            } catch (...) {
                break;
            }
        }
        auto delay = std::chrono::duration_cast<std::chrono::duration<double>>(std::chrono::steady_clock::now() - start);
        fmt::print("Wrote {} MiBs in {:.3f} seconds\n", bytes_sent >> 20, delay.count());
        out.close().handle_exception([] (auto x) {}).get();
        s.shutdown_input();
        BOOST_CHECK_LT(delay.count(), 1.0);
    });

    seastar::when_all(std::move(c), std::move(s)).discard_result().get();
}

}

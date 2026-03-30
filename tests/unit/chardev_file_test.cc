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
 * Copyright (C) 2026 ScyllaDB
 */

#include <sys/ioctl.h>
#include <termios.h>

#include <seastar/core/file.hh>
#include <seastar/core/format.hh>
#include <seastar/core/fstream.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/sstring.hh>
#include <seastar/testing/test_case.hh>
#include <seastar/testing/thread_test_case.hh>

using namespace seastar;

// Open a pseudo-terminal pair via open_file_dma.
//
// /dev/ptmx does not support O_DIRECT; open_file_dma forgives this when
// strict_o_direct is false (the default), so both ends open as
// chardev_file_impl objects.  All PTY setup (unlock, number query, raw mode)
// is performed through the file::ioctl_short() interface — no raw fd needed.
static std::pair<file, file> open_pty_pair() {
    // Open master PTY (/dev/ptmx allocates a new master on every open).
    auto master = open_file_dma("/dev/ptmx", open_flags::rw).get();

    // Unlock the slave (equivalent to unlockpt(3)).
    // grantpt(3) is a no-op on modern Linux with udev.
    int lock = 0;
    master.ioctl_short(TIOCSPTLCK, &lock).get();

    // Obtain the PTY index (equivalent to ptsname(3)).
    unsigned int pty_num = 0;
    master.ioctl_short(TIOCGPTN, &pty_num).get();
    auto slave_path = format("/dev/pts/{}", pty_num);

    // Open slave PTY.
    auto slave = open_file_dma(slave_path, open_flags::rw).get();

    // Put the slave in raw mode so data flows through the line discipline
    // unmodified (no echo, no canonical buffering, no output translation).
    struct termios t;
    slave.ioctl_short(TCGETS, &t).get();
    ::cfmakeraw(&t);
    slave.ioctl_short(TCSETS, &t).get();

    return {std::move(master), std::move(slave)};
}

// Stream data through a pseudo-terminal pair using file_output_stream on the
// slave end (program-output direction) and file_input_stream on the master end
// (terminal-emulator-read direction).  Verifies the payload arrives intact.
SEASTAR_THREAD_TEST_CASE(chardev_pty_stream_roundtrip) {
    auto [master, slave] = open_pty_pair();

    // Writer: slave writes → data flows to master (program-output direction).
    file_output_stream_options out_opts;
    out_opts.buffer_size  = 4096;
    out_opts.write_behind = 1;  // serialise writes; char devices are sequential

    // Reader: no read-ahead (we control exactly when data arrives).
    file_input_stream_options in_opts;
    in_opts.buffer_size = 4096;
    in_opts.read_ahead  = 0;

    auto out = make_file_output_stream(std::move(slave),  out_opts).get();
    auto in  = make_file_input_stream (std::move(master), in_opts);

    const sstring payload = "Hello, PTY streams!";

    out.write(payload).get();
    out.flush().get();

    auto buf = in.read_exactly(payload.size()).get();
    BOOST_REQUIRE_EQUAL(sstring(buf.get(), buf.size()), payload);

    // Close streams; this also closes the underlying file objects and fds.
    out.close().get();
    in.close().get();
}

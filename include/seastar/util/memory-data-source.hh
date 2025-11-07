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

#pragma once

#include <seastar/core/iostream.hh>

namespace seastar {
namespace util {

template <typename Container>
requires std::is_nothrow_move_constructible_v<Container>
class memory_data_source final : public data_source_impl {
    Container _bufs;
    Container::iterator _cur;

public:
    explicit memory_data_source(Container&& b) noexcept
        : _bufs(std::move(b))
        , _cur(_bufs.begin())
    {}

    virtual future<temporary_buffer<char>> get() override {
        return make_ready_future<temporary_buffer<char>>(_cur != _bufs.end() ? std::move(*_cur++) : temporary_buffer<char>());
    }
};

template <>
class memory_data_source<temporary_buffer<char>> final : public data_source_impl {
    temporary_buffer<char> _buf;

public:
    explicit memory_data_source(temporary_buffer<char>&& b) noexcept
        : _buf(std::move(b))
    {}

    virtual future<temporary_buffer<char>> get() override {
        return make_ready_future<temporary_buffer<char>>(std::exchange(_buf, {}));
    }
};

template <typename T>
requires requires (T t) { memory_data_source(std::move(t)); }
inline input_stream<char> as_input_stream(T&& bufs) {
    return input_stream<char>(data_source(std::make_unique<memory_data_source<T>>(std::move(bufs))));
}

} // util namespace
} // seastar namespace

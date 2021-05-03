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

#pragma once

namespace seastar {

/// \cond internal
class io_queue;
using io_priority_class_id = unsigned;
// We could very well just add the name to the io_priority_class. However, because that
// structure is passed along all the time - and sometimes we can't help but copy it, better keep
// it lean. The name won't really be used for anything other than monitoring.
class io_priority_class {
    io_priority_class_id _id;

    io_priority_class() = delete;
    explicit io_priority_class(io_priority_class_id id) noexcept
        : _id(id)
    { }

public:
    io_priority_class_id id() const noexcept {
        return _id;
    }

    static io_priority_class register_one(sstring name, uint32_t shares);

    bool rename(sstring name);
    unsigned get_shares() const;
    sstring get_name() const;

private:
    struct class_info {
        unsigned shares = 0;
        sstring name;
        bool registered() const noexcept { return shares != 0; }
    };

    static constexpr unsigned _max_classes = 2048;
    static std::mutex _register_lock;
    static std::array<class_info, _max_classes> _infos;
};

const io_priority_class& default_priority_class();

[[deprecated("Use engine().rename_priority_class()")]]
future<> rename_priority_class(io_priority_class pc, sstring new_name);

} // namespace seastar

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

#include <boost/container/small_vector.hpp>
#include <type_traits>
#include <seastar/util/concepts.hh>

namespace seastar {
namespace util {

/// A left-continuous staircase function
template <typename T, unsigned Inline = 2>
SEASTAR_CONCEPT( requires std::is_arithmetic_v<T> )
class staircase {
    boost::container::small_vector<std::pair<size_t, T>, Inline> _steps;
    template <typename U, unsigned V>
    friend std::ostream& operator<<(std::ostream&, const staircase<U, V>&);

public:
    /// Makes the staircase function with \c def as the axis-wide value
    staircase(T def) noexcept {
        _steps.emplace_back(std::numeric_limits<size_t>::max(), def);
    }

    /// Finds the value corresponding to \c len
    T at(size_t len) const noexcept {
        for (auto&& s : _steps) {
            if (len <= s.first) {
                return s.second;
            }
        }

        std::abort(); // cannot get here, the last element is always max
    }

    /// Adds a step at length \c len with the value \c val
    void add_step(size_t len, T val) {
        assert(len < std::numeric_limits<size_t>::max());
        auto pos = _steps.begin();
        while (pos->first < len) {
            pos++; // will hit the last max element in the worst case
        }
        _steps.emplace(pos, len, val);
    }

    /// Changes the default value to \c val
    void set_default(T val) noexcept {
        _steps.back().second = val;
    }

    T get_default() const noexcept {
        return _steps.back().second;
    }
};

template <typename T, unsigned Inline>
inline std::ostream& operator<<(std::ostream& out, const staircase<T, Inline>& st) {
    out << "staircase[";
    for (auto&& s : st._steps) {
        if (s == st._steps.back()) {
            out << " *:" << s.second;
        } else {
            out << " " << s.first << ":" << s.second;
        }
    }
    out << " ]";
    return out;
}

} // namespace util
} // namespace seastar

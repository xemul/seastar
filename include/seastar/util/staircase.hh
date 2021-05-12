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
namespace internal {

/// Left-continuous staricase function implementation
/// The function is y = F(x) = { F_i if x <= x_i } or F_default
template <typename T, unsigned Inline = 2>
SEASTAR_CONCEPT( requires std::is_arithmetic_v<T> )
class staircase {
    boost::container::small_vector<std::pair<size_t, T>, Inline> _steps;
    template <typename U, unsigned V>
    friend std::ostream& operator<<(std::ostream&, const staircase<U, V>&);

public:
    /// Constructs the staircase function with \c def as the default value
    staircase(T def) noexcept {
        _steps.emplace_back(std::numeric_limits<size_t>::max(), def);
    }

    /// Returns the value corresponding to \c x
    T operator()(size_t x) const noexcept {
        for (auto&& s : _steps) {
            if (x <= s.first) {
                return s.second;
            }
        }

        __builtin_unreachable(); // cannot get here, the last element is always max
    }

    /// Adds a step at length \c x with the value \c y
    void add_step(size_t x, T y) {
        if (x == std::numeric_limits<size_t>::max()) {
            throw std::overflow_error("Cannot add step at the end of the x-range");
        }

        auto pos = _steps.begin();
        while (pos->first < x) {
            pos++; // will hit the last max element in the worst case
        }
        _steps.emplace(pos, x, y);
    }

    /// Changes the default value to \c y
    void set_default(T y) noexcept {
        _steps.back().second = y;
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

} // namespace internal
} // namespace seastar

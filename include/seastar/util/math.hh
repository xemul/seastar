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
 * Copyright (C) 2021 ScyllaDB Ltd.
 */

namespace seastar {
namespace util {

// The std::exp family is not very peeky wrt float/double/long-double
// types, it's easy to mistake and call the float returning one with
// large enough value and overflow (infinite) it.
//
// Below are type-safe exp helpers.

template <typename T>
inline auto exp(T val) noexcept;

template <>
inline auto exp<float>(float val) noexcept {
    return expf(val);
}

template <>
inline auto exp<double>(double val) noexcept {
    return exp(val);
}

template <>
inline auto exp<long double>(long double val) noexcept {
    return expl(val);
}

} // namespace util
} // namespace seastar

/*
 * Copyright 2026 Robomous
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "roadmaker/road/bridge.hpp"

#include "roadmaker/tol.hpp"

#include <cstddef>
#include <optional>
#include <span>

namespace roadmaker {

std::optional<std::size_t> bridge_covering(std::span<const Bridge> bridges, double s) {
  for (std::size_t i = 0; i < bridges.size(); ++i) {
    // Inclusive at both ends, with the length tolerance the span operations
    // already use: a crossing exactly at the deck edge is carried by that deck.
    if (s >= bridges[i].s - tol::kLength && s <= bridges[i].s + bridges[i].length + tol::kLength) {
      return i;
    }
  }
  return std::nullopt;
}

} // namespace roadmaker

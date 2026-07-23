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

// Exercises one symbol from each exported area so a missed RM_API annotation
// fails this link even if the kernel's own tests pass.

#include <array>
#include <cstdio>
#include <roadmaker/road/authoring.hpp>
#include <roadmaker/road/network.hpp>
#include <roadmaker/version.hpp>
#include <roadmaker/xodr/writer.hpp>
#include <string>

int main() {
  rm::RoadNetwork network;
  const std::array<rm::Waypoint, 2> waypoints{rm::Waypoint{0.0, 0.0}, rm::Waypoint{50.0, 10.0}};
  const auto road =
      rm::author_clothoid_road(network, waypoints, rm::LaneProfile::two_lane_default());
  if (!road || !network.road(*road)) {
    std::fprintf(stderr, "authoring failed\n");
    return 1;
  }
  const auto xml = rm::write_xodr(network);
  if (!xml || xml->empty()) {
    std::fprintf(stderr, "writer failed\n");
    return 1;
  }
  std::printf("roadmaker %s consume smoke OK (%zu bytes of xodr)\n",
              std::string(rm::version()).c_str(),
              xml->size());
  return 0;
}

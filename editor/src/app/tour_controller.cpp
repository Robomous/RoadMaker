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

#include "app/tour_controller.hpp"

#include <QObject>
#include <utility>

namespace roadmaker::editor {

TourController::TourController(std::vector<TourStep> steps) : steps_(std::move(steps)) {}

void TourController::start() {
  index_ = 0;
  completed_ = false;
  if (steps_.empty()) {
    active_ = false;
    completed_ = true;
    return;
  }
  active_ = true;
}

const TourStep* TourController::current() const {
  if (!active_ || index_ >= steps_.size()) {
    return nullptr;
  }
  return &steps_[index_];
}

void TourController::next() {
  if (!active_) {
    return;
  }
  if (index_ + 1 >= steps_.size()) {
    active_ = false;
    completed_ = true;
    return;
  }
  ++index_;
}

void TourController::skip() {
  if (!active_) {
    return;
  }
  active_ = false;
  completed_ = true;
}

std::vector<TourStep> default_tour_steps() {
  return {
      TourStep{QObject::tr("Draw a road"),
               QObject::tr("Pick the Road tool, then click to drop waypoints and press Enter. "
                           "Roads are smooth clothoids — the geometry stays drivable."),
               QStringLiteral("Road")},
      TourStep{QObject::tr("Drag in an intersection"),
               QObject::tr("Open the Library and drag a T- or X-intersection onto the canvas to "
                           "drop a ready-made junction."),
               QStringLiteral("Library")},
      TourStep{QObject::tr("Plant a tree"),
               QObject::tr("Still in the Library, drag a tree from Props onto a road — it snaps "
                           "alongside as an OpenDRIVE object."),
               QStringLiteral("Library")},
      TourStep{QObject::tr("Shape the elevation"),
               QObject::tr("The Elevation tool opens the Profile dock: drag the height handles, "
                           "or cross one road over another for an overpass."),
               QStringLiteral("Elevation")},
      TourStep{QObject::tr("Export for a simulator"),
               QObject::tr("Export the scene as glTF (or USD) — road surfaces, junctions, and "
                           "props travel with it. Save keeps the editable .xodr."),
               QStringLiteral("Export")},
  };
}

} // namespace roadmaker::editor

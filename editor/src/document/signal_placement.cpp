#include "document/signal_placement.hpp"

#include "roadmaker/road/network.hpp"
#include "roadmaker/road/object.hpp"
#include "roadmaker/road/road.hpp"

#include <array>
#include <set>
#include <string>
#include <utility>

#include "viewport/picking.hpp" // station_to_world

namespace roadmaker::editor {

std::optional<RoadStation> nearest_signal_station(const RoadNetwork& network, double x, double y) {
  return nearest_road_station(network, x, y, kSignalSnapThreshold);
}

std::optional<std::array<double, 3>> signal_world(const RoadNetwork& network, SignalId id) {
  const Signal* signal = network.signal(id);
  if (signal == nullptr) {
    return std::nullopt;
  }
  const Road* road = network.road(signal->road);
  if (road == nullptr || road->plan_view.empty()) {
    return std::nullopt;
  }
  const std::array<double, 2> plan = station_to_world(road->plan_view, signal->s, signal->t);
  return std::array<double, 3>{plan[0], plan[1], signal->z_offset};
}

std::string next_signal_odr_id(const RoadNetwork& network) {
  std::set<std::string> taken;
  network.for_each_signal([&](SignalId, const Signal& signal) { taken.insert(signal.odr_id); });
  int candidate = 1;
  while (taken.contains(std::to_string(candidate))) {
    ++candidate;
  }
  return std::to_string(candidate);
}

bool is_signal_asset(const LibraryItem& item) {
  return item.kind == LibraryItem::Kind::Signal && !item.signal.isEmpty();
}

Signal make_signal(const QString& tag, std::string odr_id, double s, double t) {
  Signal signal;
  signal.odr_id = std::move(odr_id);
  signal.orientation = ObjectOrientation::Plus;
  signal.s = s;
  signal.t = t;
  if (tag == QStringLiteral("light")) {
    signal.dynamic = true;
    signal.type = "1000001"; // OpenDRIVE traffic-light catalog type
    signal.subtype = "-1";
    signal.country = "OpenDRIVE";
  } else if (tag == QStringLiteral("sign_stop")) {
    signal.dynamic = false;
    signal.type = "206"; // StVO 206: Halt! Vorfahrt gewähren — STOP
    signal.subtype = "-1";
    signal.country = "DE";
  } else if (tag == QStringLiteral("sign_yield")) {
    signal.dynamic = false;
    signal.type = "205"; // StVO 205: Vorfahrt gewähren — yield/give way
    signal.subtype = "-1";
    signal.country = "DE";
  } else if (tag == QStringLiteral("sign_text")) {
    // StVO 310 Ortstafel (town-entrance plate): the spec's own @text example.
    // Placed with a recognisable default the user edits in the Attributes panel.
    signal.dynamic = false;
    signal.type = "310";
    signal.subtype = "-1";
    signal.country = "DE";
    signal.text = "City";
  } else { // "sign" — generic regulatory plate (speed-limit 50)
    signal.dynamic = false;
    signal.type = "274"; // German regulatory speed-limit sign
    signal.subtype = "50";
    signal.country = "DE";
  }
  return signal;
}

} // namespace roadmaker::editor

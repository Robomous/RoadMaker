#include "document/signal_placement.hpp"

#include "roadmaker/road/network.hpp"
#include "roadmaker/road/object.hpp"

#include <set>
#include <string>
#include <utility>

namespace roadmaker::editor {

std::optional<RoadStation> nearest_signal_station(const RoadNetwork& network, double x, double y) {
  return nearest_road_station(network, x, y, kSignalSnapThreshold);
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
  } else { // "sign" — generic regulatory plate (speed-limit 50)
    signal.dynamic = false;
    signal.type = "274"; // German regulatory speed-limit sign
    signal.subtype = "50";
    signal.country = "DE";
  }
  return signal;
}

} // namespace roadmaker::editor

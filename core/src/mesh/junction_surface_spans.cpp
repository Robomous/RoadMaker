#include "junction_fill_spans.hpp"

#include "roadmaker/road/junction.hpp"
#include "roadmaker/road/road.hpp"

#include <clipper2/clipper.h>

#include <algorithm>
#include <utility>
#include <vector>

#include "junction_corner_detail.hpp"

namespace roadmaker::junction_fill_spans {

std::vector<JunctionFillSpan> collect_fill_spans(const RoadNetwork& network,
                                                 const Junction& junction,
                                                 const SamplingOptions& sampling) {
  std::vector<JunctionFillSpan> spans;
  for (const RoadId road_id : junction_corner_detail::connecting_roads(junction)) {
    const Road* road = network.road(road_id);
    if (road == nullptr || road->plan_view.empty() || road->sections.empty()) {
      continue;
    }
    fill_backend::RoadContribution contribution =
        fill_backend::build_contribution(network, *road, sampling);
    // The ring is built left-border-forward + right-border-reversed, which
    // winds CLOCKWISE — fine for NonZero union, but InflatePaths would
    // erode it (hole semantics). The weld inflation needs CCW.
    if (Clipper2Lib::Area(contribution.footprint) < 0.0) {
      std::ranges::reverse(contribution.footprint);
    }
    JunctionFillSpan span{.road = road_id, .contribution = std::move(contribution)};
    // A record whose road is no longer a connecting road is simply never looked
    // up, which is what makes it dormant rather than an error.
    const auto record = std::ranges::find_if(
        junction.surface_spans, [&](const SurfaceSpan& entry) { return entry.road == road_id; });
    if (record != junction.surface_spans.end()) {
      span.included = record->included;
      span.sort_index = record->sort_index;
      span.authored = true;
    }
    spans.push_back(std::move(span));
  }
  return spans;
}

} // namespace roadmaker::junction_fill_spans

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

/// OpenSCENARIO XML reader (p8-s1, issue #245) — the twin of
/// core/src/xodr/reader.cpp, holding the same disciplines and one correction
/// to a claim its sibling writer makes.
///
///   1. NEVER SILENTLY DROP INPUT. Every attribute with no typed field and
///      every unmodeled child element is captured verbatim into the owning
///      struct's `RawXml`, in document order, AND reported. A `.xosc` this
///      version cannot fully model still survives a round trip.
///   2. A PARSE FAILS ONLY ON A STRUCTURAL PROBLEM — malformed XML, a missing
///      <OpenSCENARIO> root, or a <Catalog> document. Everything else is a
///      Diagnostic, because losing a whole scenario over one bad number is a
///      worse outcome than loading it with a warning.
///   3. RULE IDS COME FROM THE TEXT, NOT THE NAME. See
///      `kPhaseDurationNonNegative` for the canonical example.
///
/// ★ WHAT "ORDER IS THE SCHEMA" ACTUALLY MEANS, measured rather than assumed.
/// core/src/osc/writer.cpp:20-24 says the OpenSCENARIO XSD sequences are
/// ordered, so appending preserved children last is correctness. That is true
/// of most of the standard but not all of it: the 1.4.0 class reference
/// declares 184 types `sequence`, 56 `choice` and **56 `all`**. Of the types
/// modeled here, `Vehicle`, `Pedestrian`, `BoundingBox` and `CatalogLocations`
/// are `all`, where child order is free and re-emitting a preserved child last
/// is legal by construction. Every other modeled type is a `sequence` whose
/// unmodeled children (`<ObjectController>` after `<EntityObject>`,
/// `<UsedArea>` after `<TrafficSignals>`, `<TrafficSignalGroupState>` after
/// `<TrafficSignalState>`, `<EntitySelection>` after `<ScenarioObject>`)
/// genuinely do sort last — checked one by one, not assumed.
///
/// Reference: ASAM OpenSCENARIO XML 1.4.0 §6.11, §7, §10.10 and Annex C. The
/// specification text is NOT tracked in this repository — it carries no
/// redistribution grant, unlike OpenDRIVE (third_party/asam/README.md).

#include "roadmaker/osc/reader.hpp"

#include "roadmaker/osc/rules.hpp"

#include <fmt/format.h>
#include <pugixml.hpp>

#include <fast_float/fast_float.h>

#include <cmath>
#include <cstddef>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace roadmaker::osc {
namespace {

// --- scalars ----------------------------------------------------------------

/// Locale-independent `double` parsing; rejects trailing garbage and
/// non-finite results.
///
/// Copied from core/src/xodr/reader.cpp:53-71 rather than shared, for the same
/// reason `num()` is copied in this format's writer (osc/writer.cpp:59-63): the
/// two standards' scalar policies are independent and either may need to
/// diverge. OpenSCENARIO additionally admits `$parameter` expressions in
/// numeric attributes (§9), which this reader does NOT evaluate — such a value
/// fails here and takes the preserve-the-spelling path below, which is the
/// correct outcome for a value whose meaning is only known at runtime.
std::optional<double> to_double(std::string_view text) {
  const char* first = text.data();
  const char* last = text.data() + text.size();
  double value{};
  const auto result = fast_float::from_chars(first, last, value);
  if (result.ec != std::errc{}) {
    return std::nullopt;
  }
  for (const char* p = result.ptr; p != last; ++p) {
    if (*p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
      return std::nullopt;
    }
  }
  if (!std::isfinite(value)) {
    return std::nullopt;
  }
  return value;
}

/// Strict non-negative integer parsing for `@revMajor` / `@revMinor`.
std::optional<int> to_revision(std::string_view text) {
  if (text.empty() || text.size() > 4) {
    return std::nullopt;
  }
  int value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') {
      return std::nullopt;
    }
    value = (value * 10) + (c - '0');
  }
  return value;
}

/// Serializes a node as a self-contained XML fragment, for the preserved tier.
///
/// `pugi::format_raw` drops the indentation the source document happened to
/// carry, which is why a preserved fragment comes back re-canonicalized rather
/// than byte-identical — fmt-s2's caveat (#326), stated here because this is
/// where it originates.
std::string node_to_string(const pugi::xml_node& node) {
  std::ostringstream out;
  node.print(out, "", pugi::format_raw);
  return out.str();
}

bool is_one_of(std::string_view name, std::initializer_list<std::string_view> known) {
  for (const std::string_view candidate : known) {
    if (name == candidate) {
      return true;
    }
  }
  return false;
}

std::optional<PhaseSemantics> to_semantics(std::string_view text) {
  if (text == "attention_go") {
    return PhaseSemantics::AttentionGo;
  }
  if (text == "attention_stop") {
    return PhaseSemantics::AttentionStop;
  }
  if (text == "caution") {
    return PhaseSemantics::Caution;
  }
  if (text == "fallback") {
    return PhaseSemantics::Fallback;
  }
  if (text == "go") {
    return PhaseSemantics::Go;
  }
  if (text == "stop") {
    return PhaseSemantics::Stop;
  }
  return std::nullopt;
}

class Parser {
public:
  explicit Parser(std::string_view source_name) : source_(source_name) {}

  Expected<XoscParseResult> run(const pugi::xml_document& doc) {
    const pugi::xml_node root = doc.child("OpenSCENARIO");
    if (!root) {
      return make_error(
          ErrorCode::InvalidDocument, "missing <OpenSCENARIO> root element", std::string(source_));
    }
    // A catalog document shares the root element with a scenario and differs
    // only in which child follows the header (§9.3). Refusing it by name beats
    // returning an empty Scenario that looks like a successful load of nothing.
    if (root.child("Catalog")) {
      return make_error(ErrorCode::InvalidDocument,
                        "this is an OpenSCENARIO catalog, not a scenario; catalogs are not "
                        "modeled by this version",
                        std::string(source_));
    }

    parse_header(root.child("FileHeader"));
    parse_parameter_declarations(root.child("ParameterDeclarations"));
    parse_catalog_locations(root.child("CatalogLocations"));
    parse_road_network(root.child("RoadNetwork"));
    parse_entities(root.child("Entities"));
    parse_storyboard(root.child("Storyboard"));

    // Root attributes and unmodeled root children. <ParameterDeclarations> and
    // the rest are consumed above; anything else — <MonitorDeclarations>
    // (1.4.0), a vendor extension — rides the preserved tier.
    capture_attributes(root, result_.scenario.preserved, {}, "OpenSCENARIO");
    for (const pugi::xml_node child : root.children()) {
      if (!is_one_of(child.name(),
                     {"FileHeader",
                      "ParameterDeclarations",
                      "CatalogLocations",
                      "RoadNetwork",
                      "Entities",
                      "Storyboard"})) {
        preserve_child(child, result_.scenario.preserved, "OpenSCENARIO");
      }
    }

    return std::move(result_);
  }

private:
  // --- diagnostics ---------------------------------------------------------

  /// `rule` is the ASAM checker-rule UID (roadmaker/osc/rules.hpp) when a
  /// normative rule applies. `road`/`lane` stay empty: a scenario document has
  /// no arena handle to stamp, and inventing one would be a lie about which
  /// network it refers to.
  void
  diag(Severity severity, std::string location, std::string message, std::string_view rule = {}) {
    result_.diagnostics.push_back(Diagnostic{.severity = severity,
                                             .location = std::move(location),
                                             .message = std::move(message),
                                             .rule_id = std::string(rule),
                                             .road = {},
                                             .lane = {}});
  }

  /// A required element is absent. Cited against the schema rule, because
  /// "shall comply with the schema of the detected version" is exactly what an
  /// absent 1..1 element breaks.
  void warn_missing_required(const std::string& element, const std::string& location) {
    diag(Severity::Warning,
         location,
         fmt::format("required element <{}> is missing; a default was substituted", element),
         rules::kValidSchema);
  }

  /// An unmodeled child, preserved verbatim. Reported ONCE per element name so
  /// a large foreign document produces a readable diagnostic list rather than
  /// one entry per occurrence — the `warn_unsupported` policy
  /// (core/src/xodr/reader.cpp:220-226), with the wording corrected: nothing
  /// here is "ignored", it is kept.
  void preserve_child(const pugi::xml_node& child, RawXml& preserved, const std::string& location) {
    preserved.children.push_back(node_to_string(child));
    const std::string name = child.name();
    if (warned_elements_.insert(name).second) {
      diag(Severity::Warning,
           location,
           fmt::format("element <{}> is not modeled by this version and was preserved verbatim",
                       name));
    }
  }

  /// Every attribute of `node` not in `known`, in document order.
  ///
  /// `location` is unused when nothing is unknown, which is the common case;
  /// it exists so a caller that wants a diagnostic can add one. Unknown
  /// ATTRIBUTES are deliberately silent where unknown ELEMENTS are not: an
  /// attribute is a scalar that round-trips exactly, while an element may carry
  /// behaviour this version will not apply.
  static void capture_attributes(const pugi::xml_node& node,
                                 RawXml& preserved,
                                 std::initializer_list<std::string_view> known,
                                 const std::string& /*location*/) {
    for (const pugi::xml_attribute attr : node.attributes()) {
      if (!is_one_of(attr.name(), known)) {
        preserved.attributes.emplace_back(attr.name(), attr.value());
      }
    }
  }

  /// Attribute as double.
  ///
  /// ★ A MALFORMED VALUE IS REPORTED WITH ITS SPELLING IN THE MESSAGE AND NOT
  /// CAPTURED INTO THE PRESERVED TIER. That looks like a violation of the
  /// never-drop rule and is the opposite: the writer emits modeled attributes
  /// and then preserved attributes with no de-duplication
  /// (core/src/xodr/writer.cpp:486-488 and every peer), so routing the
  /// unparseable spelling into `preserved.attributes` would emit the attribute
  /// TWICE — and a duplicate attribute is not well-formed XML, which turns one
  /// bad number into an unreadable file. The same trap `osc/scenario.hpp`
  /// documents for `@state` and `@semantics`, met from the reader's side.
  double attr_double(const pugi::xml_node& node,
                     const char* name,
                     const std::string& location,
                     double fallback = 0.0,
                     bool required = true) {
    const pugi::xml_attribute attr = node.attribute(name);
    if (!attr) {
      if (required) {
        diag(Severity::Warning,
             location,
             fmt::format("missing attribute '{}', using {}", name, fallback));
      }
      return fallback;
    }
    if (const auto value = to_double(attr.value())) {
      return *value;
    }
    diag(
        Severity::Warning,
        location,
        fmt::format(
            "attribute '{}' is not a valid number ('{}'), using {}", name, attr.value(), fallback));
    return fallback;
  }

  std::optional<double>
  attr_optional_double(const pugi::xml_node& node, const char* name, const std::string& location) {
    const pugi::xml_attribute attr = node.attribute(name);
    if (!attr) {
      return std::nullopt;
    }
    if (const auto value = to_double(attr.value())) {
      return value;
    }
    diag(Severity::Warning,
         location,
         fmt::format("attribute '{}' is not a valid number ('{}'), ignored", name, attr.value()));
    return std::nullopt;
  }

  static std::string attr_text(const pugi::xml_node& node, const char* name) {
    return node.attribute(name).value();
  }

  // --- header --------------------------------------------------------------

  /// `<FileHeader>` — §7.2.2.
  ///
  /// `@revMajor`/`@revMinor` are recorded on the RESULT and never on the model:
  /// the writer re-derives them from the target revision, so preserving them
  /// would emit each twice. Same discipline as `header_attribute_is_owned`
  /// (core/src/xodr/reader.cpp:316-319).
  void parse_header(const pugi::xml_node& header) {
    if (!header) {
      warn_missing_required("FileHeader", "FileHeader");
      return;
    }

    const pugi::xml_attribute major = header.attribute("revMajor");
    const pugi::xml_attribute minor = header.attribute("revMinor");
    if (const auto value = to_revision(major.value())) {
      result_.rev_major = *value;
    }
    if (const auto value = to_revision(minor.value())) {
      result_.rev_minor = *value;
    }
    if (result_.rev_major != 1) {
      diag(Severity::Warning,
           "FileHeader",
           fmt::format("OpenSCENARIO revision {}.{} is outside the supported 1.x range",
                       result_.rev_major,
                       result_.rev_minor));
    }

    FileHeader& out = result_.scenario.header;
    out.author = attr_text(header, "author");
    out.date = attr_text(header, "date");
    out.description = attr_text(header, "description");
    capture_attributes(header,
                       out.preserved,
                       {"revMajor", "revMinor", "author", "date", "description"},
                       "FileHeader");
    for (const pugi::xml_node child : header.children()) {
      preserve_child(child, out.preserved, "FileHeader");
    }
  }

  /// `<ParameterDeclarations>` — §9.2.
  void parse_parameter_declarations(const pugi::xml_node& parameters) {
    if (!parameters) {
      return; // 0..*, and the writer emits the wrapper unconditionally.
    }
    for (const pugi::xml_node child : parameters.children()) {
      if (std::string_view(child.name()) != "ParameterDeclaration") {
        // The wrapper itself has no struct (nothing but declarations is legal
        // inside it), so a foreign child is preserved on the SCENARIO and
        // therefore re-emitted one level up, as a sibling of the wrapper. Said
        // out loud in the diagnostic rather than hidden, because the
        // alternative is dropping it.
        diag(Severity::Warning,
             "ParameterDeclarations",
             fmt::format("element <{}> is not legal inside <ParameterDeclarations>; it was "
                         "preserved at the document level",
                         child.name()));
        result_.scenario.preserved.children.push_back(node_to_string(child));
        continue;
      }
      ParameterDeclaration out;
      out.name = attr_text(child, "name");
      out.parameter_type = attr_text(child, "parameterType");
      out.value = attr_text(child, "value");
      capture_attributes(
          child, out.preserved, {"name", "parameterType", "value"}, "ParameterDeclaration");
      for (const pugi::xml_node grandchild : child.children()) {
        preserve_child(grandchild, out.preserved, "ParameterDeclaration");
      }
      result_.scenario.parameter_declarations.push_back(std::move(out));
    }
  }

  /// `<CatalogLocations>` — preserved whole; nothing inside it is modeled
  /// until catalogs exist as a feature.
  void parse_catalog_locations(const pugi::xml_node& catalogs) {
    if (!catalogs) {
      warn_missing_required("CatalogLocations", "CatalogLocations");
      return;
    }
    capture_attributes(catalogs, result_.scenario.catalog_locations, {}, "CatalogLocations");
    for (const pugi::xml_node child : catalogs.children()) {
      // No per-name diagnostic: the whole element is preserved-only by design,
      // so every child here is expected to be unmodeled and warning about each
      // would be noise rather than information.
      result_.scenario.catalog_locations.children.push_back(node_to_string(child));
    }
  }

  // --- road network --------------------------------------------------------

  std::optional<FileRef>
  parse_file_ref(const pugi::xml_node& node, const std::string& location, bool& already_seen) {
    if (already_seen) {
      // §8.5's <geoReference> precedent (core/src/xodr/reader.cpp:357-368):
      // the first definition wins, because picking a later one would make the
      // road network depend on document order.
      diag(Severity::Warning,
           location,
           fmt::format("more than one <{}>; the first was used and the rest ignored", node.name()));
      return std::nullopt;
    }
    already_seen = true;

    FileRef out;
    out.filepath = attr_text(node, "filepath");
    capture_attributes(node, out.preserved, {"filepath"}, location);
    for (const pugi::xml_node child : node.children()) {
      preserve_child(child, out.preserved, location);
    }
    return out;
  }

  void parse_road_network(const pugi::xml_node& road_network) {
    RoadNetworkRef& out = result_.scenario.road_network;
    if (!road_network) {
      // NOT cited against the schema rule: `road_network_reference` states in
      // full that "the scenario is also valid without a defined road network",
      // so an absent <RoadNetwork> is the advisory case, not a schema breach.
      diag(Severity::Warning,
           "RoadNetwork",
           "the scenario declares no road network; a <LogicFile> reference should be present",
           rules::kRoadNetworkReference);
      return;
    }

    capture_attributes(road_network, out.preserved, {}, "RoadNetwork");

    bool saw_logic = false;
    bool saw_scene_graph = false;
    for (const pugi::xml_node child : road_network.children()) {
      const std::string_view name = child.name();
      if (name == "LogicFile") {
        if (auto file = parse_file_ref(child, "RoadNetwork/LogicFile", saw_logic)) {
          out.logic_file = std::move(file);
        }
      } else if (name == "SceneGraphFile") {
        if (auto file = parse_file_ref(child, "RoadNetwork/SceneGraphFile", saw_scene_graph)) {
          out.scene_graph_file = std::move(file);
        }
      } else if (name == "TrafficSignals") {
        parse_traffic_signals(child, out);
      } else {
        // <UsedArea> and anything foreign. Both sort after <TrafficSignals> in
        // the RoadNetwork sequence, so re-emitting last stays schema-valid.
        preserve_child(child, out.preserved, "RoadNetwork");
      }
    }

    if (!out.logic_file.has_value()) {
      diag(Severity::Warning,
           "RoadNetwork",
           "no <LogicFile> reference; the scenario names no road network",
           rules::kRoadNetworkReference);
    }
  }

  /// `<TrafficSignals>` — §6.11.
  ///
  /// The wrapper has no struct of its own and cannot get one: `signals` is a Qt
  /// macro, so a member of that name makes the header unparseable from any
  /// editor TU (osc/scenario.hpp:39-45). Controllers therefore hang directly
  /// off `RoadNetworkRef`, and a foreign child of the wrapper is preserved one
  /// level up — reported, for the same reason as in
  /// `parse_parameter_declarations`.
  void parse_traffic_signals(const pugi::xml_node& signals_node, RoadNetworkRef& out) {
    for (const pugi::xml_node child : signals_node.children()) {
      if (std::string_view(child.name()) != "TrafficSignalController") {
        diag(Severity::Warning,
             "RoadNetwork/TrafficSignals",
             fmt::format("element <{}> is not legal inside <TrafficSignals>; it was preserved "
                         "at the <RoadNetwork> level",
                         child.name()));
        out.preserved.children.push_back(node_to_string(child));
        continue;
      }
      out.traffic_signal_controllers.push_back(parse_controller(child));
    }
  }

  TrafficSignalController parse_controller(const pugi::xml_node& node) {
    const std::string location =
        fmt::format("RoadNetwork/TrafficSignals/TrafficSignalController[{}]", controller_index_++);

    TrafficSignalController out;
    // @name is the OpenDRIVE controller @id (§10.10), not a readable label —
    // the file is read exactly as it is written.
    out.name = attr_text(node, "name");
    out.delay = attr_optional_double(node, "delay", location);
    out.reference = attr_text(node, "reference");
    capture_attributes(node, out.preserved, {"name", "delay", "reference"}, location);

    std::size_t phase_index = 0;
    for (const pugi::xml_node child : node.children()) {
      if (std::string_view(child.name()) == "Phase") {
        out.phases.push_back(
            parse_phase(child, fmt::format("{}/Phase[{}]", location, phase_index++)));
      } else {
        preserve_child(child, out.preserved, location);
      }
    }
    return out;
  }

  Phase parse_phase(const pugi::xml_node& node, const std::string& location) {
    Phase out;
    out.name = attr_text(node, "name");
    // NON-negative per the rule's TEXT: zero is legal, only a negative
    // duration is a finding, and that finding belongs to validate_scenario.
    out.duration = attr_double(node, "duration", location);

    const pugi::xml_attribute semantics = node.attribute("semantics");
    if (semantics) {
      out.semantics = to_semantics(semantics.value());
      if (!out.semantics.has_value()) {
        // An unknown spelling stays unset and rides the preserved tier, which
        // is what stops the writer emitting `semantics=` twice
        // (osc/scenario.hpp:148-154). It is then re-emitted at every target
        // revision, including 1.2 where the attribute did not yet exist — the
        // general cost of the preserved tier, accepted here because dropping a
        // value the author wrote is the worse failure.
        diag(Severity::Warning,
             location,
             fmt::format("phase semantics '{}' is not a TrafficSignalSemantics literal; it was "
                         "preserved verbatim",
                         semantics.value()));
      }
    }

    capture_attributes(
        node,
        out.preserved,
        out.semantics.has_value()
            ? std::initializer_list<std::string_view>{"name", "duration", "semantics"}
            : std::initializer_list<std::string_view>{"name", "duration"},
        location);

    std::size_t state_index = 0;
    for (const pugi::xml_node child : node.children()) {
      if (std::string_view(child.name()) == "TrafficSignalState") {
        out.signal_states.push_back(parse_signal_state(
            child, fmt::format("{}/TrafficSignalState[{}]", location, state_index++)));
      } else {
        // ★ <TrafficSignalGroupState> lands here on purpose. It carries one
        // whitespace-separated state list and NO signal ids at all, so
        // normalizing it into per-signal states would invent the identity fact
        // ADR-0014 §5 exists to protect. Preserved verbatim, never interpreted.
        preserve_child(child, out.preserved, location);
      }
    }
    return out;
  }

  TrafficSignalState parse_signal_state(const pugi::xml_node& node, const std::string& location) {
    TrafficSignalState out;
    out.traffic_signal_id = attr_text(node, "trafficSignalId");
    // @state is a free string, deliberately not an enum: §10.10 admits a
    // composite value such as "on;off;off" for a whole-box signal.
    out.state = attr_text(node, "state");
    capture_attributes(node, out.preserved, {"trafficSignalId", "state"}, location);
    for (const pugi::xml_node child : node.children()) {
      preserve_child(child, out.preserved, location);
    }
    return out;
  }

  // --- entities ------------------------------------------------------------

  /// `<BoundingBox>` — §7.4.2, with `<Center>` and `<Dimensions>` flattened in.
  ///
  /// ★ AN UNKNOWN ATTRIBUTE ON <Center> OR <Dimensions> IS LIFTED to the
  /// bounding box's own preserved tier, and reported saying so. Neither
  /// container is modeled as a struct — both are pure containers in every
  /// revision — so there is nowhere else to keep it, and the alternative is a
  /// silent drop. Only schema-invalid input can reach this: both types are
  /// closed sequences of required scalars.
  BoundingBox parse_bounding_box(const pugi::xml_node& node, const std::string& location) {
    BoundingBox out;
    capture_attributes(node, out.preserved, {}, location);

    for (const pugi::xml_node child : node.children()) {
      const std::string_view name = child.name();
      if (name == "Center") {
        out.center_x = attr_double(child, "x", location + "/Center");
        out.center_y = attr_double(child, "y", location + "/Center");
        out.center_z = attr_double(child, "z", location + "/Center");
        lift_unknown_attributes(child, out.preserved, {"x", "y", "z"}, location);
      } else if (name == "Dimensions") {
        out.width = attr_double(child, "width", location + "/Dimensions");
        out.length = attr_double(child, "length", location + "/Dimensions");
        out.height = attr_double(child, "height", location + "/Dimensions");
        lift_unknown_attributes(child, out.preserved, {"width", "length", "height"}, location);
      } else {
        preserve_child(child, out.preserved, location);
      }
    }
    return out;
  }

  void lift_unknown_attributes(const pugi::xml_node& node,
                               RawXml& preserved,
                               std::initializer_list<std::string_view> known,
                               const std::string& location) {
    for (const pugi::xml_attribute attr : node.attributes()) {
      if (is_one_of(attr.name(), known)) {
        continue;
      }
      preserved.attributes.emplace_back(attr.name(), attr.value());
      diag(Severity::Warning,
           location,
           fmt::format("attribute '{}' on <{}> is not modeled; it was preserved on the enclosing "
                       "<BoundingBox>, because <{}> is flattened into it",
                       attr.name(),
                       node.name(),
                       node.name()));
    }
  }

  Axle parse_axle(const pugi::xml_node& node, const std::string& location) {
    Axle out;
    out.max_steering = attr_double(node, "maxSteering", location);
    out.wheel_diameter = attr_double(node, "wheelDiameter", location);
    out.track_width = attr_double(node, "trackWidth", location);
    out.position_x = attr_double(node, "positionX", location);
    out.position_z = attr_double(node, "positionZ", location);
    capture_attributes(node,
                       out.preserved,
                       {"maxSteering", "wheelDiameter", "trackWidth", "positionX", "positionZ"},
                       location);
    for (const pugi::xml_node child : node.children()) {
      preserve_child(child, out.preserved, location);
    }
    return out;
  }

  Axles parse_axles(const pugi::xml_node& node, const std::string& location) {
    Axles out;
    capture_attributes(node, out.preserved, {}, location);
    for (const pugi::xml_node child : node.children()) {
      const std::string_view name = child.name();
      if (name == "FrontAxle") {
        out.front = parse_axle(child, location + "/FrontAxle");
      } else if (name == "RearAxle") {
        out.rear = parse_axle(child, location + "/RearAxle");
      } else if (name == "AdditionalAxle") {
        out.additional.push_back(parse_axle(child, location + "/AdditionalAxle"));
      } else {
        preserve_child(child, out.preserved, location);
      }
    }
    return out;
  }

  /// `<Properties>` — §7.4. The wrapper's own unmodeled content rides
  /// `properties_preserved` on the owning entity rather than the entity's main
  /// tier, so a `<File>` inside `<Properties>` (which esmini's own catalogs
  /// carry) is re-emitted INSIDE `<Properties>` and not as its sibling. The
  /// `InitActions` rationale (osc/scenario.hpp:386-388), met a second time.
  void parse_properties(const pugi::xml_node& node,
                        std::vector<Property>& out,
                        RawXml& wrapper,
                        const std::string& location) {
    capture_attributes(node, wrapper, {}, location);
    for (const pugi::xml_node child : node.children()) {
      if (std::string_view(child.name()) != "Property") {
        preserve_child(child, wrapper, location);
        continue;
      }
      Property property;
      property.name = attr_text(child, "name");
      property.value = attr_text(child, "value");
      capture_attributes(child, property.preserved, {"name", "value"}, location);
      for (const pugi::xml_node grandchild : child.children()) {
        preserve_child(grandchild, property.preserved, location);
      }
      out.push_back(std::move(property));
    }
  }

  Vehicle parse_vehicle(const pugi::xml_node& node, const std::string& location) {
    Vehicle out;
    out.name = attr_text(node, "name");
    out.category = attr_text(node, "vehicleCategory");
    out.mass = attr_optional_double(node, "mass", location);
    out.model3d = attr_text(node, "model3d");
    capture_attributes(
        node, out.preserved, {"name", "vehicleCategory", "mass", "model3d"}, location);

    for (const pugi::xml_node child : node.children()) {
      const std::string_view name = child.name();
      if (name == "BoundingBox") {
        out.bounding_box = parse_bounding_box(child, location + "/BoundingBox");
      } else if (name == "Performance") {
        out.performance.max_speed = attr_double(child, "maxSpeed", location + "/Performance");
        out.performance.max_acceleration =
            attr_double(child, "maxAcceleration", location + "/Performance");
        out.performance.max_deceleration =
            attr_double(child, "maxDeceleration", location + "/Performance");
        capture_attributes(child,
                           out.performance.preserved,
                           {"maxSpeed", "maxAcceleration", "maxDeceleration"},
                           location + "/Performance");
      } else if (name == "Axles") {
        out.axles = parse_axles(child, location + "/Axles");
      } else if (name == "Properties") {
        parse_properties(child, out.properties, out.properties_preserved, location + "/Properties");
      } else {
        // <ParameterDeclarations>, <Trailer>, <TrailerHitch>... Vehicle is an
        // `all` model group, so re-emitting these after <Properties> is legal
        // by construction rather than by luck.
        preserve_child(child, out.preserved, location);
      }
    }
    return out;
  }

  Pedestrian parse_pedestrian(const pugi::xml_node& node, const std::string& location) {
    Pedestrian out;
    out.name = attr_text(node, "name");
    out.category = attr_text(node, "pedestrianCategory");
    out.mass = attr_double(node, "mass", location);
    out.model3d = attr_text(node, "model3d");
    // @model (without the 3d) is the deprecated 1.0 spelling; it is not
    // recognized here, so it rides the preserved tier and survives unchanged.
    capture_attributes(
        node, out.preserved, {"name", "pedestrianCategory", "mass", "model3d"}, location);

    for (const pugi::xml_node child : node.children()) {
      const std::string_view name = child.name();
      if (name == "BoundingBox") {
        out.bounding_box = parse_bounding_box(child, location + "/BoundingBox");
      } else if (name == "Properties") {
        parse_properties(child, out.properties, out.properties_preserved, location + "/Properties");
      } else {
        preserve_child(child, out.preserved, location);
      }
    }
    return out;
  }

  void parse_entities(const pugi::xml_node& entities) {
    Entities& out = result_.scenario.entities;
    if (!entities) {
      warn_missing_required("Entities", "Entities");
      return;
    }
    capture_attributes(entities, out.preserved, {}, "Entities");

    std::size_t index = 0;
    for (const pugi::xml_node child : entities.children()) {
      if (std::string_view(child.name()) != "ScenarioObject") {
        // <EntitySelection> sorts after <ScenarioObject> in the sequence.
        preserve_child(child, out.preserved, "Entities");
        continue;
      }
      out.scenario_objects.push_back(parse_scenario_object(child, index++));
    }
  }

  ScenarioObject parse_scenario_object(const pugi::xml_node& node, std::size_t index) {
    const std::string location = fmt::format("Entities/ScenarioObject[{}]", index);

    ScenarioObject out;
    out.name = attr_text(node, "name");
    capture_attributes(node, out.preserved, {"name"}, location);

    for (const pugi::xml_node child : node.children()) {
      const std::string_view name = child.name();
      if (name == "Vehicle") {
        out.entity_object = parse_vehicle(child, location + "/Vehicle");
      } else if (name == "Pedestrian") {
        out.entity_object = parse_pedestrian(child, location + "/Pedestrian");
      } else {
        // ★ The EntityObject choice has five arms (§7.4); this version models
        // two. A <MiscObject>, <CatalogReference> or <ExternalObjectReference>
        // leaves `entity_object` as std::monostate AND rides the preserved
        // tier — which is exactly what the variant's monostate arm is for
        // (osc/scenario.hpp:335-339). <ObjectController> reaches the same
        // branch and is equally correct there: it sorts after the entity
        // object in the ScenarioObject sequence.
        preserve_child(child, out.preserved, location);
      }
    }
    return out;
  }

  // --- storyboard ----------------------------------------------------------

  WorldPosition parse_world_position(const pugi::xml_node& node, const std::string& location) {
    WorldPosition out;
    out.x = attr_double(node, "x", location);
    out.y = attr_double(node, "y", location);
    out.z = attr_double(node, "z", location, 0.0, false);
    out.h = attr_optional_double(node, "h", location);
    out.p = attr_optional_double(node, "p", location);
    out.r = attr_optional_double(node, "r", location);
    capture_attributes(node, out.preserved, {"x", "y", "z", "h", "p", "r"}, location);
    for (const pugi::xml_node child : node.children()) {
      preserve_child(child, out.preserved, location);
    }
    return out;
  }

  /// One `<PrivateAction>` — a ten-way choice (§7.5) of which this version
  /// models exactly one arm, and only when its position is a world position.
  ///
  /// ★ A <TeleportAction> whose <Position> is a <LanePosition> (or any of the
  /// other nine position types) is preserved WHOLE, with `teleport` left unset.
  /// Modeling the teleport and defaulting the position would silently move the
  /// entity to the origin — a file that parses, writes and simulates, with the
  /// actor in the wrong place. `TeleportAction::position` is not optional
  /// precisely so this decision has to be made here.
  PrivateAction parse_private_action(const pugi::xml_node& node, const std::string& location) {
    PrivateAction out;
    capture_attributes(node, out.preserved, {}, location);

    for (const pugi::xml_node child : node.children()) {
      const bool is_teleport = std::string_view(child.name()) == "TeleportAction";
      const pugi::xml_node world =
          is_teleport ? child.child("Position").child("WorldPosition") : pugi::xml_node{};
      if (!world) {
        if (is_teleport) {
          // Worth its own wording: "<TeleportAction> is not modeled" would be
          // wrong and would send a reader looking in the wrong place. The
          // ACTION is modeled; its position is one of ten types and only the
          // world position is.
          const pugi::xml_node position = child.child("Position").first_child();
          diag(Severity::Warning,
               location,
               fmt::format("<TeleportAction> uses <{}>, and only <WorldPosition> is modeled; "
                           "the whole action was preserved verbatim rather than moved to the "
                           "origin",
                           position ? position.name() : "Position"));
          out.preserved.children.push_back(node_to_string(child));
          continue;
        }
        preserve_child(child, out.preserved, location);
        continue;
      }

      TeleportAction teleport;
      teleport.position = parse_world_position(world, location + "/TeleportAction/WorldPosition");
      capture_attributes(child, teleport.preserved, {}, location);
      for (const pugi::xml_node grandchild : child.children()) {
        if (std::string_view(grandchild.name()) != "Position") {
          preserve_child(grandchild, teleport.preserved, location + "/TeleportAction");
        }
      }
      out.teleport = std::move(teleport);
    }
    return out;
  }

  void parse_init(const pugi::xml_node& node, Init& out) {
    capture_attributes(node, out.preserved, {}, "Storyboard/Init");

    for (const pugi::xml_node child : node.children()) {
      if (std::string_view(child.name()) != "Actions") {
        preserve_child(child, out.preserved, "Storyboard/Init");
        continue;
      }
      capture_attributes(child, out.actions.preserved, {}, "Storyboard/Init/Actions");

      std::size_t index = 0;
      for (const pugi::xml_node action : child.children()) {
        if (std::string_view(action.name()) != "Private") {
          // ★ THE ONE PLACE WHERE APPENDING PRESERVED CHILDREN LAST IS NOT
          // SCHEMA-SAFE, and it is worth being exact about. `InitActions` is a
          // sequence of globalActions*, privates*, userDefinedActions* — so a
          // preserved <UserDefinedAction> lands correctly after the privates,
          // while a <GlobalAction> is REORDERED past them. Only the latter gets
          // the extra diagnostic; modeling either is p8-s4's.
          if (std::string_view(action.name()) == "GlobalAction") {
            diag(Severity::Warning,
                 "Storyboard/Init/Actions",
                 "<GlobalAction> is not modeled and was preserved after the <Private> actions, "
                 "which moves it out of its schema position within <Actions>");
            out.actions.preserved.children.push_back(node_to_string(action));
          } else {
            preserve_child(action, out.actions.preserved, "Storyboard/Init/Actions");
          }
          continue;
        }
        const std::string location = fmt::format("Storyboard/Init/Actions/Private[{}]", index++);

        Private entry;
        entry.entity_ref = attr_text(action, "entityRef");
        capture_attributes(action, entry.preserved, {"entityRef"}, location);
        for (const pugi::xml_node private_child : action.children()) {
          if (std::string_view(private_child.name()) == "PrivateAction") {
            entry.actions.push_back(parse_private_action(private_child, location));
          } else {
            preserve_child(private_child, entry.preserved, location);
          }
        }
        out.actions.privates.push_back(std::move(entry));
      }
    }
  }

  void parse_trigger(const pugi::xml_node& node, Trigger& out, const std::string& location) {
    capture_attributes(node, out.preserved, {}, location);

    for (const pugi::xml_node group_node : node.children()) {
      if (std::string_view(group_node.name()) != "ConditionGroup") {
        preserve_child(group_node, out.preserved, location);
        continue;
      }
      ConditionGroup group;
      capture_attributes(group_node, group.preserved, {}, location);

      for (const pugi::xml_node condition_node : group_node.children()) {
        if (std::string_view(condition_node.name()) != "Condition") {
          preserve_child(condition_node, group.preserved, location);
          continue;
        }
        Condition condition;
        condition.name = attr_text(condition_node, "name");
        condition.delay = attr_double(condition_node, "delay", location, 0.0, false);
        condition.condition_edge = attr_text(condition_node, "conditionEdge");
        capture_attributes(
            condition_node, condition.preserved, {"name", "delay", "conditionEdge"}, location);

        for (const pugi::xml_node child : condition_node.children()) {
          const pugi::xml_node simulation_time =
              std::string_view(child.name()) == "ByValueCondition"
                  ? child.child("SimulationTimeCondition")
                  : pugi::xml_node{};
          if (!simulation_time) {
            // A <ByEntityCondition>, or a <ByValueCondition> holding one of the
            // other value conditions. Preserved whole at the <Condition> level,
            // which is where the wrapper actually sits, so no fragment moves.
            preserve_child(child, condition.preserved, location);
            continue;
          }
          SimulationTimeCondition timing;
          timing.value = attr_double(simulation_time, "value", location);
          timing.rule = attr_text(simulation_time, "rule");
          capture_attributes(simulation_time, timing.preserved, {"value", "rule"}, location);
          condition.simulation_time = std::move(timing);
        }
        group.conditions.push_back(std::move(condition));
      }
      out.condition_groups.push_back(std::move(group));
    }
  }

  void parse_storyboard(const pugi::xml_node& storyboard) {
    Storyboard& out = result_.scenario.storyboard;
    if (!storyboard) {
      warn_missing_required("Storyboard", "Storyboard");
      return;
    }
    capture_attributes(storyboard, out.preserved, {}, "Storyboard");

    bool saw_init = false;
    for (const pugi::xml_node child : storyboard.children()) {
      const std::string_view name = child.name();
      if (name == "Init") {
        saw_init = true;
        parse_init(child, out.init);
      } else if (name == "Story") {
        // Its own vector rather than the generic preserved tier, so the writer
        // re-emits it in its schema slot between <Init> and <StopTrigger>
        // (core/src/osc/writer.cpp:424-427) instead of after the stop trigger.
        out.preserved_stories.push_back(node_to_string(child));
      } else if (name == "StopTrigger") {
        parse_trigger(child, out.stop_trigger, "Storyboard/StopTrigger");
      } else {
        preserve_child(child, out.preserved, "Storyboard");
      }
    }
    if (!saw_init) {
      warn_missing_required("Init", "Storyboard/Init");
    }
  }

  std::string_view source_;
  XoscParseResult result_;
  std::set<std::string> warned_elements_;
  std::size_t controller_index_ = 0;
};

} // namespace

Expected<XoscParseResult> parse_xosc(std::string_view xml_text, std::string_view source_name) {
  pugi::xml_document doc;
  const pugi::xml_parse_result parsed = doc.load_buffer(xml_text.data(), xml_text.size());
  if (!parsed) {
    return make_error(ErrorCode::MalformedXml,
                      fmt::format("XML parse error: {}", parsed.description()),
                      std::string(source_name));
  }
  return Parser(source_name).run(doc);
}

Expected<XoscParseResult> load_xosc(const std::filesystem::path& path) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec) || ec) {
    return make_error(ErrorCode::FileNotFound, "file not found", path.string());
  }
  // Binary mode: the parser owns newline handling (CRLF must survive).
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return make_error(ErrorCode::IoFailure, "could not open file", path.string());
  }
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  const std::string text = std::move(buffer).str();

  auto result = parse_xosc(text, path.string());
  if (!result) {
    return result;
  }

  // The two findings only a PATH can support. They live here rather than in
  // validate_scenario because a Scenario knows neither its own file name nor
  // the directory a relative filepath resolves against — the same split that
  // puts the terrain sidecar's resolution in load_xodr
  // (core/src/xodr/reader.cpp:3260+).
  if (path.extension() != ".xosc") {
    result->diagnostics.push_back(
        Diagnostic{.severity = Severity::Warning,
                   .location = path.string(),
                   .message = "scenario descriptions should have the file extension '.xosc'",
                   .rule_id = std::string(rules::kFileEnding)});
  }

  const auto check_resolvable = [&](const std::optional<FileRef>& file, const char* element) {
    if (!file.has_value() || file->filepath.empty()) {
      return;
    }
    const std::filesystem::path referenced = path.parent_path() / file->filepath;
    std::error_code exists_ec;
    if (!std::filesystem::exists(referenced, exists_ec) || exists_ec) {
      result->diagnostics.push_back(Diagnostic{
          .severity = Severity::Warning,
          .location = fmt::format("RoadNetwork/{}", element),
          .message = fmt::format("filepath '{}' does not resolve to a file next to this scenario",
                                 file->filepath),
          .rule_id = std::string(rules::kRoadNetworkAvailability)});
    }
  };
  check_resolvable(result->scenario.road_network.logic_file, "LogicFile");
  check_resolvable(result->scenario.road_network.scene_graph_file, "SceneGraphFile");

  return result;
}

} // namespace roadmaker::osc

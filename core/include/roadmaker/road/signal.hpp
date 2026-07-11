#pragma once

#include "roadmaker/road/id.hpp"
#include "roadmaker/road/object.hpp" // ObjectOrientation (shared e_orientation)
#include "roadmaker/xodr/raw_xml.hpp"

#include <optional>
#include <string>

namespace roadmaker {

/// <signal> (OpenDRIVE 1.9.0 §14.1, Table 122). A traffic sign, traffic
/// light, or specific road marking that controls or regulates traffic
/// (items that do not influence traffic models are <object>s instead, §14.1).
/// Placement is in road s/t/zOffset, resolved to world through the owning
/// road's reference line + elevation. Owned by RoadNetwork's signal arena;
/// `road` is the back-reference mirroring Object::road / Lane::section.
///
/// Optionality follows the schema (as-built Phase 0 rule, design §9): an
/// attribute optional in Table 122 stays std::optional so an absent value is
/// never invented on write. Everything §14 does not model — <validity>,
/// <dependency>, <reference>, <userData>, boards — is preserved verbatim in
/// `preserved` (never dropped, design §5).
struct Signal {
  RoadId road; ///< owning road (back-reference)

  std::string odr_id; ///< <signal @id> — required, unique in file (string)
  std::string name;   ///< optional

  double s = 0.0, t = 0.0; ///< origin in road coordinates (required, §14.1)
  double z_offset = 0.0;   ///< relative to reference-line elevation (required)

  /// @dynamic yes/no (required): dynamic (traffic light) vs. static (sign).
  /// Optional so an absent attribute round-trips rather than defaulting to a
  /// spelled value; the reader warns when it is missing.
  std::optional<bool> dynamic;

  /// e_orientation (required, §14.1): the travel direction the signal is
  /// valid for. Reuses Object's ObjectOrientation (same e_orientation enum).
  ObjectOrientation orientation = ObjectOrientation::None;

  double h_offset = 0.0, pitch = 0.0, roll = 0.0; ///< optional angles [rad]

  std::string type;             ///< required; country-coded (or "-1"/"none"), §14.1
  std::string subtype;          ///< required; country-coded (or "-1"/"none")
  std::string country;          ///< e_countryCode; "OpenDRIVE" for catalog signals
  std::string country_revision; ///< year the applied traffic rules came into force

  std::optional<double> value; ///< e.g. 50 for a speed limit; unit then required
  std::string unit;            ///< e_unit; mandatory iff @value present (§14.1)
  std::string text;            ///< optional free text on the signal

  std::optional<double> height, width; ///< bounding-box dimensions [m]
  std::optional<double> length;        ///< 1.8.0; emitted only when target >=1.8.0

  std::optional<bool> temporary;   ///< 1.9.0
  std::optional<bool> invalidated; ///< 1.9.0

  /// Unmodeled children (<validity>, <dependency>, <reference>, <userData>,
  /// boards) and unknown attributes — preserved verbatim per the never-drop
  /// contract (docs/design/m3a/01 §5).
  RawXml preserved;
};

} // namespace roadmaker

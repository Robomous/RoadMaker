#pragma once

#include "roadmaker/xodr/raw_xml.hpp"

#include <optional>
#include <string>
#include <vector>

namespace roadmaker {

/// <control> (OpenDRIVE 1.9.0 §14.6, Table 129; identical in 1.8.1 §14.6).
/// "Provides information about a single signal within a signal group controlled
/// by the corresponding controller." Multiplicity inside <controller> is 1..*.
///
/// The signal is referenced by its STRING @signalId, not by a SignalId: that is
/// what the standard stores, it stays faithful to a dangling reference in
/// third-party input, and it survives the signal being erased and restored by
/// the command layer. Resolving it to a live signal is a query's job, never the
/// model's.
struct Control {
  std::string signal_odr_id; ///< @signalId — required, id of the controlled signal
  std::string type;          ///< @type — optional, free text, application-defined
};

/// <controller> (OpenDRIVE 1.9.0 §14.6, Table 128; identical in 1.8.1 §14.6).
/// A signal group: "the mapping of dynamic signals to a signal group is done in
/// <controller>". The signal cycle itself is deliberately outside OpenDRIVE
/// (§14.6 names ASAM OpenSCENARIO), so nothing here carries timing.
///
/// TOP-LEVEL: `<controller>` is a child of `<OpenDRIVE>` (§14.6, and the element
/// overview Table 12 places it after every `<road>` and before the first
/// `<junction>`), so it is owned by no road and no junction. A junction only
/// REFERENCES it, through JunctionController (§12.14).
///
/// Optionality follows the schema, as with Signal: an attribute optional in
/// Table 128 stays std::optional so an absent value is never invented on write.
/// Everything §14.6 does not model is preserved verbatim in `preserved` (the
/// never-drop contract, docs/design/m3a/01 §5).
struct Controller {
  std::string odr_id;               ///< @id — required, unique within database
  std::string name;                 ///< @name — optional, freely chosen
  std::optional<unsigned> sequence; ///< @sequence — optional nonNegativeInteger

  /// <control> children (1..*). A controller with none is a validator finding
  /// (asam.net:xodr:1.7.0:road.signal.controller.valid_for_signals), never a
  /// silent drop.
  std::vector<Control> controls;

  /// Unknown attributes and unmodeled children, preserved verbatim.
  RawXml preserved;
};

} // namespace roadmaker

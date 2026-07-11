#pragma once

#include <algorithm>
#include <array>
#include <string_view>

/// GS-1 signal catalog (docs/design/m3a/01 §3, maintainer decision #34.1).
///
/// M3a ships NO signal-catalog data file: the authored palette is exactly the
/// three GS-1 signals, hard-coded here as type/subtype/country constants. The
/// kernel keeps this table so validation and (phase 5) the editor placement UI
/// read one source of truth; country catalogs are backlog. Any other
/// type/subtype still parses into the Modeled tier and round-trips — it simply
/// has no palette entry (design §3).
///
/// Sources: GS-1 golden scene + OpenDRIVE 1.9.0 §14.1 XML example (DE codes)
/// and the ASAM Signal reference 1.0.0 (country="OpenDRIVE" traffic lights).
namespace roadmaker::signal_catalog {

/// One authored GS-1 signal palette entry.
struct CatalogEntry {
  std::string_view key;     ///< stable UI/label key
  bool dynamic;             ///< @dynamic
  std::string_view type;    ///< @type
  std::string_view subtype; ///< @subtype
  std::string_view country; ///< @country
  std::string_view unit;    ///< @unit ("" when the signal carries no @value)
};

/// The GS-1 authored palette (design §3, Table). Order is UI order.
inline constexpr std::array<CatalogEntry, 3> kEntries{{
    // Dynamic traffic light — Signal reference, country="OpenDRIVE".
    {.key = "traffic_light",
     .dynamic = true,
     .type = "1000001",
     .subtype = "-1",
     .country = "OpenDRIVE",
     .unit = ""},
    // Static speed limit 50 km/h — DE catalog (§14.1 example is type 274).
    {.key = "speed_limit_50",
     .dynamic = false,
     .type = "274",
     .subtype = "50",
     .country = "DE",
     .unit = "km/h"},
    // Static pedestrian-crossing warning sign — DE catalog.
    {.key = "pedestrian_crossing",
     .dynamic = false,
     .type = "101",
     .subtype = "11",
     .country = "DE",
     .unit = ""},
}};

/// True when (type, subtype, country) is an authored GS-1 palette entry.
[[nodiscard]] constexpr bool
is_authored(std::string_view type, std::string_view subtype, std::string_view country) {
  return std::ranges::any_of(kEntries, [&](const CatalogEntry& entry) {
    return entry.type == type && entry.subtype == subtype && entry.country == country;
  });
}

} // namespace roadmaker::signal_catalog

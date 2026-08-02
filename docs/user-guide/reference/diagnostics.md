# Diagnostics

*Parser and validator findings for the current network, each citing the ASAM
rule it comes from.*

## What it shows

The **Diagnostics** panel lists the findings raised when a file is read and when
the network is validated: structural problems, values outside the standard, and
information the parser could not fully round-trip. Each finding names the object
it concerns and, where the standard defines one, cites its **ASAM rule id**
(for example `asam.net:xodr:1.4.0:ids.id_unique_in_class`) so you can look up the
exact normative text.

## Severities

- **Error** — the network violates the standard or cannot be represented
  faithfully; fix these before you rely on the export.
- **Warning** — something unusual or lossy that is still valid; worth a look.
- **Info** — a note, such as a value the parser normalised.

## Working from a finding

- **Click a finding** to select the object it refers to; the viewport and the
  [Scene tree](scene-tree.md) jump to it so you can correct it in
  [Properties](attributes.md) or with the relevant tool.
- Findings refresh as you edit: fix the cause and the entry clears on the next
  validation pass.

## Reference

The parser never silently drops input — every dropped or normalised element
becomes a structured finding. The diagnostic model and the rule-id contract are
covered in the OpenDRIVE domain notes (`docs/domain/opendrive.md`).

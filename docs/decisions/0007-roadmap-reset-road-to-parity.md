# ADR-0007: Roadmap reset — "Road to Parity" and a single release

- **Status:** ACCEPTED — maintainer decided 2026-07-15
- **Date:** 2026-07-15
- **Deciders:** Armando Anaya

## Context

Until 2026-07 the project shipped a release per milestone (v0.1.0,
v0.2.0, v0.3.0 published; v0.4.0 … v0.10.0 planned). Those were
**development checkpoints, not releases**: at none of them was the app a
usable product for its intended audience. The tags created an obligation
to publish, changelog, and gate work that had no users, while the roadmap
(M1 … M5 milestones, golden scenes GS-1 … GS-4, golden workflows GW-1/GW-2)
optimized for milestone close-outs rather than product maturity.

Separately, the maintainer set the product goal explicitly: approach the
capabilities of the strongest commercial road/scenario editors, replicating
their proven interaction model — modal tools, one Library Browser from
which everything is dragged, an Attributes pane that is a drag-target with
scrub-editing, a 2D Editor pane hosting profile/phase editors — rather
than inventing a new one. Per the
[product-parity naming rule](../standards/product-parity.md#the-naming-rule),
vendor products are internal inspiration only and are never named in this
repository.

## Decision

We reset the roadmap to **"Road to Parity"**
([docs/roadmap/README.md](../roadmap/README.md)): eight capability pillars
(P1 Interaction & Navigation … P8 Scenarios), each an epic with 1–2-week
sprint issues, accepted exclusively by hand-executed
[golden workflows](../roadmap/golden_workflows/README.md) GW-1 … GW-6.

We adopt a **single-release policy**: exactly one release, **v0.1.0**,
published only when all pillar sprints are complete, every golden workflow
passes by hand on macOS + Linux + Windows, a 24 h ASan soak is clean, docs
are synchronized, and the maintainer explicitly approves. Sprints end with
a merged PR and updated docs — no tags, no versions, no release tasks.
Only the maintainer publishes; automation and AI agents never tag or
release.

Accordingly, in the 2026-07 reset we **deleted** the published releases
(v0.1.0 … v0.3.0) and all git tags (freeing v0.1.0 for the real release),
**deleted** all GitHub milestones, closed the open issue backlog as
not-planned (re-mapping technical content into the pillar backlog), and
deleted the four issues whose entire purpose was release bookkeeping. The
old planning corpus (milestone roadmap, gap analysis, seeds, golden scenes
GS-1 … GS-4, golden workflows GW-1/GW-2 and the v0.4.0 gate record, release
notes) moved to
[docs/roadmap/archive/2026-07-pre-reset/](../roadmap/archive/2026-07-pre-reset/README.md).

## Consequences

- **Easier:** the backlog states one goal (parity) with one acceptance
  mechanism (golden workflows); no per-milestone release overhead; `main`
  is the product until v0.1.0; the pillar graph makes dependencies
  (interaction model first, library/attributes drag model early) explicit.
- **Harder:** no intermediate tags means no ready-made "known good"
  snapshots — bisection and comparisons use commit hashes; users tracking
  the project must build from source until v0.1.0.
- **History:** deleting tags/releases/milestones removed public
  checkpoints; the archive folder and the git history remain the record.
  ASSETS/CHANGELOG references to old versions stay as historical text.
- **Reversal cost:** low for the release policy (start tagging again);
  moderate for the backlog (issues would need re-grouping into milestones
  again). The archive keeps the old model restorable.

## References

- [Road to Parity roadmap](../roadmap/README.md)
- [Golden workflows](../roadmap/golden_workflows/README.md)
- [Product parity and IP rules](../standards/product-parity.md)
- [Pre-reset archive](../roadmap/archive/2026-07-pre-reset/README.md)

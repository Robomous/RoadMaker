# Golden workflows

*What golden workflows are, and why milestone acceptance now measures the
path a user takes — not only the scene they end up with.*

A **golden workflow** is a scripted sequence of user actions with a time
budget and a **zero-crash requirement**, executed **by the maintainer by
hand** at every milestone gate. Agent-run soak tests and offscreen unit
tests are complementary evidence, never a substitute for the manual run —
they exercise the command layer, not the real GL/widget/interaction
lifetimes a human session does.

## Why they exist

Golden scenes measure the **result**; golden workflows measure the
**path**. The mechanism was adopted during the hardening sprint
(v0.4.0) because scene-based acceptance had a blind spot that maintainer
dogfooding exposed: GS-1 is a 4-arm junction built from road *endpoints*,
so the impossible-in-v0.3.0 workflow of attaching a road to the *side* of
another road (a T-intersection — the second thing anyone draws) was never
exercised by any acceptance artifact. A scene checklist can be fully green
while the path to build it by hand is broken or crashes.

The standing rule that follows (recorded in the
[product-parity standard](../../standards/product-parity.md)): tool and
milestone specs must include **workflow walkthroughs** — what a user does,
step by step, in their first minutes — not only element coverage of the
standard's vocabulary.

## The workflow set

| Workflow | One-liner | Introduced |
|---|---|---|
| [GW-1 "First network"](gw1_first_network.md) | Two-lane road, T-junction by side attach, overpass, lane edit, junction-adjacent drag, undo/redo, save/reload, export + esmini | Hardening (v0.4.0) — the sprint's gate |
| [GW-2 "Recover from crash"](gw2_recover_from_crash.md) | SIGKILL mid-edit, relaunch, autosave recovery, ≤ 2 min of work lost | Hardening (v0.4.0) |

## Anatomy of a spec

Every golden workflow spec contains:

1. **Action script** — numbered user actions in tool/UI vocabulary
   (which tool, what interaction), specific enough that two runs are
   comparable.
2. **Time budget** — the whole script must complete within it, by hand.
3. **Pass criteria** — always includes *zero crashes*; adds
   workflow-specific checks (validation clean, recovery bounds, export
   loads in external tools).
4. **Evidence to record** — what the runner writes into the gate document
   (timings, diagnostics count, crash reports if any).

## Process

- **Every milestone from the hardening sprint on gates on its golden
  scene AND at least one golden workflow** (see the roadmap's
  [acceptance mechanics](../roadmap.md#acceptance-mechanics)).
- The maintainer executes the workflow(s) by hand at the gate and fills
  the milestone's gate document (e.g.
  [gate-v0.4.0.md](gate-v0.4.0.md)) during the run.
- Any crash during a gated workflow is a NO-GO: the milestone extends
  with those crashes only, each filed with the `crash` issue template.
- Workflows are versioned like scene specs: new capabilities extend the
  set (or add steps) rather than silently rewriting history.
- Where steps are automatable, the soak/regression suites replay them
  headless in CI as an early-warning signal between gates.

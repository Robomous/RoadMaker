# GW-2 — Recover from crash

*Autosave and crash recovery, proven the honest way: kill the editor and
see what a user actually gets back.*

- **Budget:** ≤ 10 minutes wall clock.
- **Pass:** zero crashes during recovery itself; recovered network loses
  **≤ 2 minutes of work** (one autosave interval); recovery is offered
  without hunting for files.

## Action script

1. Open (or build) a small network; keep editing continuously — create a
   road, move nodes, edit a lane — for at least 3 minutes so several
   autosave cycles elapse.
2. Note the last edit you made (e.g. "moved node 3 of Road 2 north").
3. **Kill the editor process with SIGKILL** (`kill -9 <pid>` /
   Task Manager "End task") mid-edit — not a clean quit; no save prompt
   may appear.
4. Relaunch the editor and open the same document (or accept the
   launch-time offer, whichever the build presents).
5. **Accept the autosave recovery offer.**
6. Verify the recovered network: everything up to at most one autosave
   interval (2 min default) before the kill is present; the document
   validates; a normal save then succeeds.

## Evidence to record (in the gate document)

- Autosave interval configured; timestamp of kill vs. timestamp of the
  autosave that was recovered.
- What (if anything) was lost, in user terms.
- Whether the recovery offer appeared unprompted, and whether the crash
  handler left a crash report for the SIGKILL session (SIGKILL is not
  catchable — *no* report is expected; the autosave sidecar is the safety
  net being tested).

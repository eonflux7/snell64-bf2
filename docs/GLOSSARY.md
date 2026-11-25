# Glossary

The project's words, the IDs and the states. `tools/docs.py check` reads the state tables below:
a state not listed here is an error.

## IDs

| ID | What | Defined in |
|---|---|---|
| `render-23` | A milestone: lane, then its number in that lane | Its document's front matter (an open `render` milestone), a heading with a `State:` line (lane documents, `ui-menu.md`), or its row in [PLAN.md](PLAN.md) once accepted |
| `render-23.7` | A task of that milestone | A heading in the milestone's document, followed by its `State:` line |
| `fix-1` | A fix, by number (`fix-A` to `fix-R` before 2026-11-25: the letter's place in the alphabet) | Its heading in [fix.md](lanes/fix.md) |
| `D-3` | An owner's decision | Its row in [DECISIONS.md](DECISIONS.md) |
| `Q-2` | A question waiting for the owner | Its heading in [STATUS.md](STATUS.md#open-questions-for-the-owner) |

IDs are never renumbered or reused. A cut task keeps its heading, state `cut`. A task that splits
keeps its number for the first part; the rest takes the milestone's next free number. Lanes:
`render`, `ui`, `tools`, `perf`, `fix`, `compat`, `dev`, `build` ([AGENTS.md](../AGENTS.md#lanes)).
Contract versions keep their numbers: "contract 46" ([contract-versions.md](render/contract-versions.md)).
Commits before 2026-11-25 name them in words ("render milestone 23, task 7"; before 2026-11-19
"step"); `tools/docs.py log render-23.7` finds both.

## Task states

In order; `deferred` and `cut` can follow any of the first three.

| State | Meaning |
|---|---|
| `proposed` | Written down, not yet agreed with the owner |
| `planned` | Agreed; not started |
| `in progress` | Being built |
| `built` | In the code (and shaders), committed, and checked by an agent in game at a view that exercises it |
| `seen` | The owner has looked at it in game; fine for now, not yet accepted |
| `accepted` | The owner accepted it; usually the whole milestone at once |
| `deferred` | Put off, with the reason or decision (`D-n`) on the `State:` line |
| `cut` | Dropped; the heading stays so the number is not reused |

## Milestone states

| State | Meaning |
|---|---|
| `proposed` | Designed or named, not scheduled |
| `planned` | Scheduled; no task started |
| `in progress` | At least one task started; sessions work on it |
| `paused` | Started, set aside by the owner's decision; its next step is known |
| `built` | Every task that was kept is built; waits for the owner's acceptance |
| `accepted` | The owner accepted it in game: its switches and old paths deleted, its document folded into the subsystem's (`render`) |
| `cut` | Dropped |

Open milestones (in progress or paused) are listed in [STATUS.md](STATUS.md), generated.

## Lane states

| State | Meaning |
|---|---|
| `active` | Work in it now |
| `paused` | Picked up again when another lane's work needs it |
| `parked` | Not worked on until the owner says so |

## Terms

| Term | Meaning |
|---|---|
| Lane | The kind of a change; every change belongs to one ([AGENTS.md](../AGENTS.md#lanes)) |
| Retail | The shipped BF2 1.50 binaries and their behaviour, which the engine reconstructs |
| PR | Project Reality, the mod this client runs; its content is private and never in this tree |
| rhooks | PR's server-side Python hooks; `compat` specifies what PR does, not their internals |
| Contract | What the shaders may rely on from the engine (constants, samplers, techniques), versioned by `RA_CONTRACT_VERSION` ([contract.md](render/contract.md)) |
| `Ra` | Prefix of the renderer's effects, techniques and semantics (`RaWater`, `RaAtmosphere`) |
| A/B switch | A temporary console variable selecting the old path while a milestone is developed; marked `[AB <milestone>]` in the code and deleted on acceptance |
| View | A saved camera on a map (`view.go NAME`); the views live outside this tree |
| Check | An agent's in-game A/B at a view that exercises a change |
| Owner | The project's maintainer; decides, looks and accepts |
| Fold | On acceptance, a milestone's document rewritten as built into its subsystem's document under [render/](render/README.md), then deleted |

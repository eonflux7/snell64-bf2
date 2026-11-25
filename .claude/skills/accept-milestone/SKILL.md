---
name: accept-milestone
description: Close a milestone the owner has accepted (render, ui or tools) - delete its A/B switches and old paths, fold its document into the subsystem's, update PLAN, STATUS and DECISIONS. Use when the owner says a milestone (render-23, ui-5, ...) is accepted.
---

# Accept a milestone

Only on the owner's word: "render-23 accepted" or the like. Agents never accept.

1. **Record it.** In the milestone's front matter (or its heading's `State:` line) set the state
   to `accepted`; every kept task's `State:` to `accepted`. Any ruling the owner gave with the
   acceptance becomes a `D-n` row in `docs/DECISIONS.md`.
2. **Delete the switches.** `tools/docs.py check` now fails on every `[AB <milestone>]` marker
   left in `Code/`: delete each switch and the old path it selects, in one commit in the
   milestone's lane (`render(<area>): milestone 23's switches and old paths deleted`, trailer
   `Milestone: render-23`). The shaders' old paths go in the private workspace's commit.
   Rebuild in the VM and check in game that the default path draws as accepted; say what was run.
3. **Fold the document** (`render` milestones). Rewrite what was built into the subsystem's
   document under `docs/render/` (its index in `docs/render/README.md` names which), as built:
   design and facts, no task history, no "planned". New subsystem: a new document and an index
   row. Move the milestone's checks' outcome there in one line. Then delete
   `docs/milestones/<file>.md`; `ui-menu.md` and lane documents keep the accepted milestone's
   heading with `State: accepted`, trimmed to what was built.
4. **PLAN.md.** The milestone's row: `Accepted (D-n if any)`, the commits
   (`tools/docs.py log <id>` lists them; a range or the list), and for a deleted document
   `git show <last commit that had it>:docs/milestones/<file>.md`.
5. **STATUS.md.** Remove its "Awaiting the owner's look" entries and any `Q-n` it settled.
   Run `tools/docs.py gen` (the open milestones table drops it) and `tools/docs.py check`.
6. **Commit** the documents: `docs: render-23 accepted`, trailer `Milestone: render-23`.

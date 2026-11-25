# Writing the documents

How `docs/` is written, for agents and people. Where each kind of fact lives: [AGENTS.md](../AGENTS.md#documentation).
Words, IDs and states: [GLOSSARY.md](GLOSSARY.md). `tools/docs.py check` enforces what it can;
the pre-commit hook runs it (`git config core.hooksPath .githooks` once per clone).

## Style

- Short sentences, one fact each. A list of facts is a bulleted list, not a sentence of semicolons.
- Name things by ID: `render-23.7`, `D-3`, `Q-2`, contract 46. Not "the task above".
- Values (defaults, ranges, sizes) go in tables when there are more than three.
- A decision by the owner is a row in [DECISIONS.md](DECISIONS.md), named here by its ID, not
  retold.
- Dates as YYYY-MM-DD. "Now", "currently", "recently" go stale: state the date or leave it out.
- State, not story: what is true now. How it got there is the commit message's.
- About 16 KB a document, 120 lines a section; past that, split or add sub-headings (`check`
  warns).
- Console names exactly as in the code (`renderer.waterFoam`); `check` warns on names the code
  does not have. All of them: [console.md](console.md), generated.

## Generated parts

Never edit them by hand; `tools/docs.py gen` rewrites them, `check` fails when they are stale.

- [STATUS.md](STATUS.md)'s open milestones table, between `<!-- gen:open-milestones -->` and
  `<!-- /gen -->`, from the milestones' front matter, headings and task states.
- The state cell of each milestone row in [PLAN.md](PLAN.md) that has a document.
- [console.md](console.md), from the code's console bindings and help strings.

## A milestone document

`docs/milestones/<lane>-<n>-<name>.md`. The front matter is the milestone's state; the task
headings are the tasks'. Fields: `id`, `title`, `state` (a milestone state), `updated`; `next`
optional (otherwise the open tasks are listed).

```markdown
---
id: render-24
title: Example
state: in progress
updated: 2026-11-25
next: 3 (the bake), the owner's answer to Q-4
---

# Render milestone 24: example

Design, tasks and checks of an open milestone; its order is in [PLAN.md](../PLAN.md#render).
On acceptance it is folded into the subsystem's document under [render/](../render/README.md)
and deleted.

Resume here: the bake's reader is half written (`FarBake.cpp`, `readHeader`); the next session
starts there.

## Design

...

## Tasks

### render-24.1 First task

State: built (2026-11-25). Contract 47.

What it does, where it lives, how it was checked.

### render-24.2 Second task

State: planned.
```

"Resume here" says exactly where a session that stopped mid-task left off; delete it when the task
is done. `ui-menu.md` and the lane documents keep milestones as headings instead
(`### ui-7 Main menu and options`) with a `State:` line, their tasks one level below.

## An entry awaiting the owner's look

In [STATUS.md](STATUS.md#awaiting-the-owners-look). Fixed fields, so it can be ticked off:

```markdown
- **Rivers' standing waves** (`render-23.7`, contract 46)
  - Where: Kunar, view `river-narrows`
  - Toggle: `renderer.waterFoam 0` the waves alone; `renderer.waterFlowSpeed 2.5` a faster river
  - Look for: crests held still in the fast shallows, foam carried downstream
  - Known: at the bend the shore break whitens the river and hides the carried foam
```

`Where:` names the map and the views; a private check confirms the views exist. The entry goes
when the owner has looked: the task's state becomes `seen` or `accepted`.

## An open question

In [STATUS.md](STATUS.md#open-questions-for-the-owner). The owner answers by ID ("Q-3: B"); the
answer becomes a decision row and the question goes.

```markdown
### Q-9 Example: which bake format

For `render-24.3`. A. One file per level (simple; 40 MB each). B. Tiles shared between
levels (smaller; a cache to manage). Recommended: A.
```

## A decision

A row in [DECISIONS.md](DECISIONS.md): ID, date, scope (an ID or a lane), the ruling in one or two
sentences. Rows are never renumbered; a decision that replaces another says "replaces D-n", and
the old row gains "(replaced by D-m)".

## Commits

The trailer names what the commit works on: `Task: render-23.7` or `Milestone: render-23`; the
`commit-msg` hook warns when a `render`, `ui` or `tools` commit has neither.
`tools/docs.py log render-23.7` lists them, with older commits that say it in words.

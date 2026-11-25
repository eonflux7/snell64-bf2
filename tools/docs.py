#!/usr/bin/env python3
"""The documentation's tooling: checks, generated tables, ID lookup.

  docs.py check          every check; exit 1 on an error (warnings do not fail)
  docs.py gen            rewrite the generated parts (STATUS, PLAN states, docs/console.md)
  docs.py find ID        where an ID (render-23, render-23.7, ui-5, fix-A, D-3, Q-2) is
                         defined and every document that names it
  docs.py log ID         the commits of a milestone or task, old wording included
  docs.py cost           what a session reads before it starts work
  docs.py hook pre-commit|commit-msg FILE
                         the git hooks (.githooks/); `git config core.hooksPath .githooks`

Rules and formats: docs/WRITING.md; the states: docs/GLOSSARY.md. Python 3 standard library only.
"""
import fnmatch
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOCS = os.path.join(ROOT, "docs")
LANES = ("render", "ui", "tools", "perf", "fix", "compat", "dev", "build")

# An ID: a milestone `render-23`, a task `render-23.7`, a fix `fix-A`, a decision `D-3`, an
# open question `Q-2`. Not followed by `-word` (file names such as render-23-water-shore.md).
ID_RE = re.compile(r"(?<![\w/.-])((?:%s)-\d+(?:\.\d+)?|fix-[A-Z]|[DQ]-\d+)(?![\w-]|\.\d)"
                   % "|".join(LANES))
MILESTONE_HEAD_RE = re.compile(r"^#{2,5} `?((?:%s)-\d+)`? (.+)$" % "|".join(LANES))
TASK_HEAD_RE = re.compile(r"^#{2,5} `?((?:%s)-\d+\.\d+)`? (.+)$" % "|".join(LANES))
FIX_HEAD_RE = re.compile(r"^#{2,5} `?(fix-[A-Z])`? (.+)$")
PLAN_ROW_RE = re.compile(r"^\| `((?:%s)-\d+)` ([^|]+)\|" % "|".join(LANES))
DECISION_ROW_RE = re.compile(r"^\| (D-\d+) \|")
QUESTION_RE = re.compile(r"^#{2,5} (Q-\d+) (.+)$")
STATE_LINE_RE = re.compile(r"^State: (.+)$")
GEN_RE = re.compile(r"(<!-- gen:([\w-]+) -->\n)(.*?)(<!-- /gen -->)", re.S)
CONSOLE_OBJECTS = ("renderer", "tools", "view", "path", "bench")
CONSOLE_REF_RE = re.compile(r"(?<![\w.])(%s)\.([A-Za-z]\w*)" % "|".join(CONSOLE_OBJECTS))
AB_RE = re.compile(r"\[AB ((?:%s)-\d+)\]" % "|".join(LANES))

SIZE_WARN = 16000      # bytes in one document
SECTION_WARN = 120     # lines under one heading
STATUS_WARN = 9000     # bytes in STATUS.md: every session reads it
LOGS = {"console.md", "contract-versions.md", "DECISIONS.md"}   # read by lookup, not whole


class Report:
    def __init__(self):
        self.errors, self.warnings = [], []

    def error(self, where, msg):
        self.errors.append("%s: %s" % (where, msg))

    def warn(self, where, msg):
        self.warnings.append("%s: %s" % (where, msg))

    def show(self):
        for w in self.warnings:
            print("warning: " + w)
        for e in self.errors:
            print("error: " + e)
        if self.errors:
            print("%d error(s); `tools/docs.py gen` fixes the generated parts" % len(self.errors))
        return 1 if self.errors else 0


def rel(path):
    return os.path.relpath(path, ROOT)


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def write(path, text):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def doc_files():
    out = []
    for base, dirs, files in os.walk(DOCS):
        dirs[:] = [d for d in dirs if d != "archive"]
        out += [os.path.join(base, f) for f in files if f.endswith(".md")]
    out += [os.path.join(ROOT, f) for f in ("AGENTS.md", "README.md")]
    return sorted(p for p in out if os.path.exists(p))


# --- the vocabulary -------------------------------------------------------------------------

def states():
    """The states, from GLOSSARY.md's tables: {'task': [...], 'milestone': [...], 'lane': [...]}."""
    text = read(os.path.join(DOCS, "GLOSSARY.md"))
    out = {}
    for kind in ("task", "milestone", "lane"):
        m = re.search(r"^## %s states\n(.*?)(?=^## |\Z)" % kind.capitalize(), text, re.M | re.S)
        out[kind] = re.findall(r"^\| `([a-z ]+)` \|", m.group(1), re.M) if m else []
    return out


def parse_state(text, vocab):
    """The state a `State:` value starts with, the longest match first; None if none."""
    low = text.lower()
    for s in sorted(vocab, key=len, reverse=True):
        if low == s or (low.startswith(s) and not low[len(s)].isalnum()):
            return s
    return None


# --- milestones and tasks --------------------------------------------------------------------

class Item:
    def __init__(self, ident, title, path, line, state_text=None, fields=None):
        self.id, self.title, self.path, self.line = ident, title.strip(), path, line
        self.state_text = state_text
        self.state = None
        self.tasks = []
        self.fields = dict(fields or {})

    @property
    def where(self):
        return "%s:%d" % (rel(self.path), self.line)


def front_matter(text):
    if not text.startswith("---\n"):
        return {}, 0
    end = text.find("\n---\n", 4)
    if end < 0:
        return {}, 0
    fields = {}
    for ln in text[4:end].splitlines():
        if ":" in ln:
            k, v = ln.split(":", 1)
            fields[k.strip()] = v.strip()
    return fields, text[:end + 5].count("\n")


def state_after(lines, i):
    """The `State:` line right after a heading (blank lines skipped)."""
    for j in range(i + 1, min(i + 4, len(lines))):
        if lines[j].strip() == "":
            continue
        m = STATE_LINE_RE.match(lines[j])
        return m.group(1) if m else None
    return None


def collect():
    """Every milestone, task, fix, decision and question defined in the documents."""
    vocab = states()
    milestones, tasks, others, plan_rows = {}, {}, {}, {}
    rep = Report()
    for path in doc_files():
        text = read(path)
        lines = text.splitlines()
        fm, _ = front_matter(text)
        if fm.get("id"):
            it = Item(fm["id"], fm.get("title", ""), path, 1, fm.get("state"), fields=fm)
            if fm["id"] in milestones:
                rep.error(it.where, "%s defined twice (also %s)" % (fm["id"], milestones[fm["id"]].where))
            milestones[fm["id"]] = it
        in_code = False
        for i, ln in enumerate(lines):
            if ln.startswith("```"):
                in_code = not in_code
            if in_code:
                continue
            for rx, table in ((TASK_HEAD_RE, tasks), (MILESTONE_HEAD_RE, milestones),
                              (FIX_HEAD_RE, others), (QUESTION_RE, others)):
                m = rx.match(ln)
                if m:
                    it = Item(m.group(1), m.group(2), path, i + 1, state_after(lines, i))
                    if m.group(1) in table:
                        rep.error(it.where, "%s defined twice (also %s)" % (m.group(1), table[m.group(1)].where))
                    table[m.group(1)] = it
                    break
            m = DECISION_ROW_RE.match(ln)
            if m:
                others[m.group(1)] = Item(m.group(1), "", path, i + 1)
            if path.endswith("PLAN.md"):
                if ln.startswith("| Milestone |"):
                    head = [c.strip() for c in ln.strip().strip("|").split("|")]
                    col = next((k for k, c in enumerate(head) if c in ("State", "Status")), len(head) - 1)
                m = PLAN_ROW_RE.match(ln)
                if m:
                    cells = [c.strip() for c in ln.strip().strip("|").split("|")]
                    plan_rows[m.group(1)] = (i + 1, cells, col)
    for it in milestones.values():
        if it.state_text is not None:
            it.state = parse_state(it.state_text, vocab["milestone"])
    for it in tasks.values():
        if it.state_text is not None:
            it.state = parse_state(it.state_text, vocab["task"])
        ms = it.id.rsplit(".", 1)[0]
        if ms in milestones:
            milestones[ms].tasks.append(it)
    # A milestone known only by its PLAN row (accepted, its document folded away).
    for ident, (line, cells, col) in plan_rows.items():
        if ident not in milestones:
            it = Item(ident, cells[0].split(" ", 1)[-1], os.path.join(DOCS, "PLAN.md"), line, cells[col])
            it.state = parse_state(cells[col], vocab["milestone"])
            it.fields["plan_only"] = True
            milestones[ident] = it
    for it in milestones.values():
        it.tasks.sort(key=lambda t: int(t.id.rsplit(".", 1)[1]))
    return milestones, tasks, others, plan_rows, vocab, rep


def ranges(nums):
    nums = sorted(nums)
    out, i = [], 0
    while i < len(nums):
        j = i
        while j + 1 < len(nums) and nums[j + 1] == nums[j] + 1:
            j += 1
        out.append(str(nums[i]) if i == j else "%d–%d" % (nums[i], nums[j]))
        i = j + 1
    return ", ".join(out)


def task_summary(ms):
    """'tasks 1–7 built, 8–9 planned' from the tasks' states, in the order of the states."""
    if not ms.tasks:
        return ""
    order = ["accepted", "seen", "built", "in progress", "planned", "proposed", "deferred", "cut"]
    vocab = order + [s for s in states()["task"] if s not in order]
    by = {}
    for t in ms.tasks:
        by.setdefault(t.state or "?", []).append(int(t.id.rsplit(".", 1)[1]))
    parts = ["%s %s" % (ranges(by[s]), s) for s in vocab + ["?"] if s in by]
    return "tasks " + ", ".join(parts)


def state_cell(ms):
    head = ms.state.capitalize() if ms.state else "?"
    summ = task_summary(ms)
    return head + (": " + summ if summ else "")


def next_cell(ms):
    if ms.fields.get("next"):
        return ms.fields["next"]
    open_ = [t for t in ms.tasks if t.state in ("proposed", "planned", "in progress")]
    return ", ".join("%s (%s)" % (t.id.rsplit(".", 1)[1], t.title.lower()) for t in open_) or "—"


def link(path, frm):
    return os.path.relpath(path, os.path.dirname(frm)).replace(os.sep, "/")


# --- generated parts ----------------------------------------------------------------------------

def gen_open_milestones(milestones, frm):
    rows = ["| Milestone | State | Next | Document |", "|---|---|---|---|"]
    order = {"in progress": 0, "paused": 1}
    open_ = [m for m in milestones.values() if m.state in order]
    open_.sort(key=lambda m: (order[m.state], LANES.index(m.id.split("-")[0]),
                              -int(m.id.split("-")[1])))
    for m in open_:
        doc = "[%s](%s)" % (os.path.basename(m.path), link(m.path, frm))
        if not m.path.endswith(os.path.join("milestones", os.path.basename(m.path))) or \
                m.fields.get("id") != m.id:
            doc = "[%s](%s#%s)" % (os.path.basename(m.path), link(m.path, frm), slug(m.id + " " + m.title))
        rows.append("| `%s` %s | %s | %s | %s |" % (m.id, m.title, state_cell(m), next_cell(m), doc))
    return "\n".join(rows) + "\n"


def slug(heading):
    h = heading.strip().lower()
    h = re.sub(r"[^\w\- ]", "", h)
    return h.replace(" ", "-")


def console_names():
    """Console objects and members this project adds, from the code: {object: {member: (kind, help, where)}}."""
    out = {o: {} for o in CONSOLE_OBJECTS}
    found = git("grep", "-l", "-i", "-E", r'BF2_CONSOLE_OBJECT\("(%s)"' % "|".join(CONSOLE_OBJECTS),
                "--", "Code")
    srcs = [os.path.join(ROOT, f) for f in found.split()]
    helps = {}
    for src in srcs:
        if not os.path.exists(src):
            continue
        text = read(src)
        for m in re.finditer(r'^\s*\{ "(\w+)", "((?:[^"\\]|\\.)*)" \},?$', text, re.M):
            helps[m.group(1)] = m.group(2).replace('\\"', '"')
        obj, in_define = None, False
        for i, ln in enumerate(text.splitlines()):
            cont = in_define
            in_define = ln.rstrip().endswith("\\") and (cont or ln.lstrip().startswith("#"))
            if cont or ln.lstrip().startswith("#"):
                continue
            m = re.match(r'BF2_CONSOLE_OBJECT\("(\w+)"', ln)
            if m:
                obj = m.group(1).lower() if m.group(1).lower() in CONSOLE_OBJECTS else None
                continue
            if not obj:
                continue
            # Any binding macro at the line's start, its first argument the member's name.
            m = re.match(r"\s*([A-Z][A-Z0-9_]*)\(\s*([A-Za-z]\w*)\s*[,)]", ln)
            if m and "ACCESSORS" not in m.group(1) and not m.group(1).startswith("BF2_CC_"):
                kind = "command" if "METHOD" in m.group(1) else "variable"
                out[obj].setdefault(m.group(2), (kind, None, "%s:%d" % (rel(src), i + 1)))
    for obj in out:
        for name, (kind, _, where) in list(out[obj].items()):
            out[obj][name] = (kind, helps.get(name) if obj == "renderer" else None, where)
    return out


def ab_markers():
    """`[AB render-23]` markers in the code: {milestone: [where, ...]}."""
    out = {}
    try:
        res = subprocess.run(["git", "grep", "-n", "-E", r"\[AB [a-z]+-[0-9]+\]", "--", "Code"],
                             cwd=ROOT, capture_output=True, text=True)
    except OSError:
        return out
    for ln in res.stdout.splitlines():
        m = AB_RE.search(ln)
        if m:
            out.setdefault(m.group(1), []).append(":".join(ln.split(":", 2)[:2]))
    return out


def gen_console():
    names = console_names()
    abs_ = ab_markers()
    lines = ["# Console reference", "",
             "Generated by `tools/docs.py gen` from the code's console bindings and help strings;",
             "do not edit. The objects the renderer and the tools use (`%s`), retail's members"
             % "`, `".join(CONSOLE_OBJECTS), "included; the help is the code's (`renderer` only).",
             "A/B switches (`[AB <milestone>]` in the code) go when their milestone is accepted.", ""]
    for obj in CONSOLE_OBJECTS:
        members = names.get(obj) or {}
        if not members:
            continue
        lines += ["## `%s`" % obj, "", "| Name | Kind | What |", "|---|---|---|"]
        for name in sorted(members, key=str.lower):
            kind, help_, where = members[name]
            what = (help_ or "").replace("|", "\\|")
            lines.append("| `%s.%s` | %s | %s |" % (obj, name, kind, what))
        lines.append("")
    if abs_:
        lines += ["## A/B switches", "", "| Milestone | Where |", "|---|---|"]
        for ms in sorted(abs_):
            files = sorted({w.split(":")[0] for w in abs_[ms]})   # no line numbers: they churn
            lines.append("| `%s` | %s |" % (ms, ", ".join("`%s`" % w for w in files)))
        lines.append("")
    return "\n".join(lines)


def generate(apply):
    """The generated parts as they should be: {path: text}; written when apply."""
    milestones, *_ = collect()
    out = {}
    status = os.path.join(DOCS, "STATUS.md")
    text = read(status)
    new = GEN_RE.sub(lambda m: m.group(1) + (gen_open_milestones(milestones, status)
                                             if m.group(2) == "open-milestones" else m.group(3))
                     + m.group(4), text)
    out[status] = new
    plan = os.path.join(DOCS, "PLAN.md")
    ptext = read(plan)
    plines = ptext.split("\n")
    col = None
    for i, ln in enumerate(plines):
        if ln.startswith("| Milestone |"):
            head = [c.strip() for c in ln.strip().strip("|").split("|")]
            col = next((k for k, c in enumerate(head) if c in ("State", "Status")), len(head) - 1)
        m = PLAN_ROW_RE.match(ln)
        if not m:
            continue
        ms = milestones.get(m.group(1))
        # Only open milestones' cells are generated; the others keep their notes and commits,
        # and `check` compares their states.
        if not ms or ms.fields.get("plan_only") or ms.state not in ("in progress", "paused"):
            continue
        cells = ln.strip().strip("|").split("|")
        cells[col] = " " + state_cell(ms) + " "
        plines[i] = "|" + "|".join(cells) + "|"
    out[plan] = "\n".join(plines)
    out[os.path.join(DOCS, "console.md")] = gen_console()
    if apply:
        for p, t in out.items():
            if not os.path.exists(p) or read(p) != t:
                write(p, t)
                print("wrote " + rel(p))
    return out


# --- checks ------------------------------------------------------------------------------------

def check_links(rep, files):
    anchors = {}
    for f in files:
        anchors[os.path.normpath(f)] = {slug(l.lstrip("#")) for l in read(f).splitlines() if l.startswith("#")}
    for f in files:
        text = re.sub(r"^```.*?^```", "", read(f), flags=re.S | re.M)
        for m in re.finditer(r"\]\(([^)\s]+)\)", text):
            t = m.group(1)
            if re.match(r"[a-z]+:", t):
                continue
            p, _, a = t.partition("#")
            tgt = os.path.normpath(os.path.join(os.path.dirname(f), p)) if p else os.path.normpath(f)
            if not os.path.exists(tgt):
                rep.error(rel(f), "link to a missing file: " + t)
            elif a and tgt.endswith(".md") and tgt in anchors and a not in anchors[tgt]:
                rep.error(rel(f), "link to a missing heading: " + t)


def check_ids(rep, files, milestones, tasks, others, vocab, plan_rows):
    known = set(milestones) | set(tasks) | set(others)
    for ms in milestones.values():
        if ms.fields.get("plan_only"):
            if ms.state is None:
                rep.error(ms.where, "%s: state %r is not a milestone state (GLOSSARY.md)" % (ms.id, ms.state_text))
            continue
        if ms.state is None:
            rep.error(ms.where, "%s has no state, or one not in GLOSSARY.md: %r" % (ms.id, ms.state_text))
        if ms.fields.get("id"):
            for k in ("title", "state", "updated"):
                if not ms.fields.get(k):
                    rep.error(ms.where, "front matter lacks `%s`" % k)
            if ms.fields.get("updated") and not re.match(r"\d{4}-\d\d-\d\d$", ms.fields["updated"]):
                rep.error(ms.where, "`updated` is not YYYY-MM-DD")
        nums = [int(t.id.rsplit(".", 1)[1]) for t in ms.tasks]
        if nums and sorted(nums) != list(range(1, max(nums) + 1)):
            rep.warn(ms.where, "%s's tasks skip a number: %s (a cut task keeps its heading)" % (ms.id, ranges(nums)))
    for t in tasks.values():
        if t.state is None:
            rep.error(t.where, "%s: no `State:` line under its heading, or a state not in GLOSSARY.md: %r"
                      % (t.id, t.state_text))
        if t.id.rsplit(".", 1)[0] not in milestones:
            rep.error(t.where, "%s belongs to no milestone" % t.id)
    for ident, (line, cells, col) in plan_rows.items():
        ms = milestones.get(ident)
        if ms and not ms.fields.get("plan_only"):
            st = parse_state(cells[col], vocab["milestone"])
            if st != ms.state:
                rep.error("docs/PLAN.md:%d" % line, "%s is %r here but %r in %s" % (ident, st, ms.state, ms.where))
    for ident, ms in milestones.items():
        if not ms.fields.get("plan_only") and ident not in plan_rows:
            rep.error(ms.where, "%s has no row in PLAN.md" % ident)
    accepted = {m for m, it in milestones.items() if it.state in ("accepted", "cut")}
    for f in files:
        if f.endswith("console.md"):
            continue
        in_code = False
        for i, ln in enumerate(read(f).splitlines()):
            if ln.startswith("```"):
                in_code = not in_code
            if in_code:
                continue
            for m in ID_RE.finditer(ln):
                ident = m.group(1)
                if ident in known:
                    continue
                parent = ident.rsplit(".", 1)[0]
                if "." in ident and parent in accepted:
                    continue           # a task of an accepted milestone: folded, history
                rep.error("%s:%d" % (rel(f), i + 1), "names %s, defined nowhere" % ident)


def check_contract(rep):
    head = os.path.join(ROOT, "Code/BF2/RendDX9/RendDX9Base.h")
    doc = os.path.join(DOCS, "render/contract-versions.md")
    m = re.search(r"#define RA_CONTRACT_VERSION (\d+)", read(head))
    rows = re.findall(r"^\| (\d+) \|", read(doc), re.M)
    if m and rows and int(m.group(1)) != max(map(int, rows)):
        rep.error(rel(doc), "the last contract row is %s, RA_CONTRACT_VERSION is %s"
                  % (max(map(int, rows)), m.group(1)))


def code_has(token):
    res = subprocess.run(["git", "grep", "-q", "-F", token, "--", "Code"], cwd=ROOT)
    return res.returncode == 0


def check_flags(rep):
    doc = os.path.join(DOCS, "lanes/dev.md")
    live, retired = set(), set()
    for para in re.split(r"\n\s*\n", read(doc)):
        names = set(re.findall(r"`(BF2_[A-Z0-9_]+)", para))
        if re.match(r"\s*(None at present.*\n)?\s*Retired", para) or "Retired," in para.split("\n")[0]:
            if "Retired" in para:
                head, _, tail = para.partition("Retired")
                live |= set(re.findall(r"^[|-] `(BF2_[A-Z0-9_]+)", head, re.M))
                retired |= set(re.findall(r"`(BF2_[A-Z0-9_]+)", tail))
                continue
        live |= set(re.findall(r"^[|-] `(BF2_[A-Z0-9_]+)", para, re.M))
    for f in sorted(live):
        if not code_has(f):
            rep.error(rel(doc), "lists %s, which the code does not read" % f)
    for f in sorted(retired):
        if code_has('"%s' % f):
            rep.warn(rel(doc), "%s is listed as retired but the code still names it" % f)


HISTORY_WORDS = re.compile(r"\b(deleted|replaced|retired|removed|dropped|gone|went|was|were|old|planned|spans?)\b", re.I)
FILE_EXTS = {"md", "dll", "exe", "cpp", "h", "txt", "py", "fx", "con", "log", "csv"}


def check_console(rep, files, milestones):
    """Console names the documents use that the code does not have. Not checked: lines that say
    the name went (history), and sections of planned or proposed milestones and tasks."""
    names = console_names()
    future = ("planned", "proposed")
    for f in files:
        if f.endswith("console.md"):
            continue
        lines = read(f).splitlines()
        skip_level = None
        for i, ln in enumerate(lines):
            h = re.match(r"^(#+) ", ln)
            if h:
                level = len(h.group(1))
                if skip_level is not None and level <= skip_level:
                    skip_level = None
                m = MILESTONE_HEAD_RE.match(ln) or TASK_HEAD_RE.match(ln)
                if m and skip_level is None:
                    st = parse_state(state_after(lines, i) or "", states()["task"] + states()["milestone"])
                    if st in future:
                        skip_level = level
            if skip_level is not None or HISTORY_WORDS.search(ln):
                continue
            for m in CONSOLE_REF_RE.finditer(ln):
                obj, mem = m.group(1), m.group(2)
                if not names.get(obj) or mem in names[obj] or mem in FILE_EXTS:
                    continue
                if ln[m.end():m.end() + 1] == "*" and any(n.startswith(mem) for n in names[obj]):
                    continue
                rep.warn("%s:%d" % (rel(f), i + 1), "%s.%s is not a console name in the code" % (obj, mem))
    for ms, wheres in ab_markers().items():
        it = milestones.get(ms)
        if it is None:
            rep.error(wheres[0], "[AB %s]: no such milestone" % ms)
        elif it.state in ("accepted", "cut"):
            rep.error(wheres[0], "[AB %s]: the milestone is %s; delete the switch and its old path"
                      % (ms, it.state))


def check_sizes(rep, files):
    for f in files:
        text = read(f)
        if len(text.encode()) > SIZE_WARN and os.path.basename(f) not in LOGS:
            rep.warn(rel(f), "%d KB (budget %d KB): split it or move history to commits"
                     % (len(text.encode()) // 1000, SIZE_WARN // 1000))
        start, head = 0, None
        lines = text.splitlines()
        for i, ln in enumerate(lines + ["# end"]):
            if ln.startswith("#"):
                if head and i - start > SECTION_WARN and not f.endswith("console.md"):
                    rep.warn("%s:%d" % (rel(f), start + 1), "section of %d lines: give it sub-headings" % (i - start))
                start, head = i, ln
    st = os.path.join(DOCS, "STATUS.md")
    if len(read(st).encode()) > STATUS_WARN:
        rep.warn(rel(st), "%d bytes (budget %d): every session reads it" % (len(read(st).encode()), STATUS_WARN))


def check_generated(rep):
    for path, text in generate(apply=False).items():
        if not os.path.exists(path) or read(path) != text:
            rep.error(rel(path), "generated part out of date: run `tools/docs.py gen`")


def run_check():
    milestones, tasks, others, plan_rows, vocab, rep = collect()
    files = doc_files()
    for kind, v in vocab.items():
        if not v:
            rep.error("docs/GLOSSARY.md", "no `## %s states` table" % kind.capitalize())
    check_links(rep, files)
    check_ids(rep, files, milestones, tasks, others, vocab, plan_rows)
    check_contract(rep)
    check_flags(rep)
    check_console(rep, files, milestones)
    check_sizes(rep, files)
    check_generated(rep)
    return rep


# --- hooks ---------------------------------------------------------------------------------------

def git(*args):
    return subprocess.run(["git"] + list(args), cwd=ROOT, capture_output=True, text=True).stdout


def docmap():
    """docs/docmap.txt: `<code glob> <doc> [<doc> ...]` per line."""
    path = os.path.join(DOCS, "docmap.txt")
    out = []
    if os.path.exists(path):
        for ln in read(path).splitlines():
            ln = ln.split("#", 1)[0].split()
            if len(ln) >= 2:
                out.append((ln[0], ln[1:]))
    return out


def private_patterns():
    """Patterns that must never reach this public tree. Built in: personal absolute paths. More
    from the file `git config eon64.privatePatterns` names (kept outside this tree), one regex a line."""
    # Split so this file does not match its own patterns.
    pats = [r"/home/[a-z_][\w-]*/", r"/run/media" + "/", r"[A-Za-z]:\\" + r"Users\\"]
    hashes = set()
    email = git("config", "--get", "user.email").strip()
    if "@" in email and "noreply" not in email:
        pats.append(re.escape(email))      # read from git, never written down
    path = git("config", "--get", "eon64.privatePatterns").strip()
    if path and os.path.exists(os.path.expanduser(path)):
        for ln in read(os.path.expanduser(path)).splitlines():
            if ln.startswith("sha256:"):
                hashes.add(ln[7:].strip().lower())
            elif ln.strip() and not ln.startswith("#"):
                pats.append(ln.strip())
    return pats, hashes, bool(path)


def hashed_hit(line, hashes):
    import hashlib
    for addr in re.findall(r"[\w.+-]+@[\w-]+(?:\.[\w-]+)+", line):
        if hashlib.sha256(addr.lower().encode()).hexdigest() in hashes:
            return True
    return False


def hook_pre_commit():
    rep = Report()
    staged = [f for f in git("diff", "--cached", "--name-only", "--diff-filter=ACMR").splitlines() if f]
    # Privacy: added lines only, so inherited text does not block a commit.
    pats, hashes, configured = private_patterns()
    if not configured:
        rep.warn("privacy", "`git config eon64.privatePatterns <file>` is unset: built-in patterns only")
    rx = re.compile("|".join("(?:%s)" % p for p in pats), re.I)
    cur = None
    for ln in git("diff", "--cached", "-U0", "--no-color").splitlines():
        if ln.startswith("+++ "):
            cur = ln[6:]
        elif ln.startswith("+") and cur and not cur.startswith("docs/archive/") and \
                (rx.search(ln) or hashed_hit(ln, hashes)):
            rep.error(cur, "a private pattern in an added line: %s" % ln[1:80])
    # Docs in the same commit as the code they describe.
    docs_staged = [f for f in staged if f.startswith("docs/")]
    for glob, docs in docmap():
        hits = [f for f in staged if fnmatch.fnmatch(f, glob)]
        if hits and not any(d in docs_staged for d in docs) and \
                not any(f.startswith("docs/milestones/") for f in docs_staged):
            rep.warn(hits[0], "changed without %s or an open milestone's document" % " or ".join(docs))
    if any(f.endswith(".md") or f.startswith("Code/") for f in staged):
        full = run_check()
        rep.errors += full.errors
        rep.warnings += full.warnings
    return rep.show()


def hook_commit_msg(path):
    msg = read(path)
    subject = msg.splitlines()[0] if msg else ""
    if re.match(r"(render|ui|tools)\(", subject) and not re.search(r"^(Milestone|Task): ", msg, re.M):
        print("warning: a %s commit without a `Task: <id>` or `Milestone: <id>` trailer"
              % subject.split("(")[0])
    for m in re.finditer(r"^(?:Milestone|Task): (\S+)", msg, re.M):
        if not ID_RE.fullmatch(m.group(1)):
            print("error: trailer %r is not an ID (render-23, render-23.7)" % m.group(0))
            return 1
    return 0


# --- lookups --------------------------------------------------------------------------------------

def cmd_find(ident):
    milestones, tasks, others, *_ = collect()
    it = milestones.get(ident) or tasks.get(ident) or others.get(ident)
    if it:
        print("defined  %s  %s%s" % (it.where, it.title, ("  [%s]" % it.state) if it.state else ""))
        for t in getattr(it, "tasks", []):
            print("  task   %s  %s  [%s]" % (t.id, t.title, t.state))
    else:
        print("not defined: " + ident)
    rx = re.compile(r"(?<![\w/.-])%s(?![\w-]|\.\d)" % re.escape(ident))
    for f in doc_files():
        for i, ln in enumerate(read(f).splitlines()):
            if rx.search(ln):
                print("named    %s:%d  %s" % (rel(f), i + 1, ln.strip()[:110]))
    return 0 if it else 1


def cmd_log(ident):
    m = re.match(r"([a-z]+)-(\d+)(?:\.(\d+))?$", ident)
    if not m:
        print("not a milestone or task ID: " + ident)
        return 1
    lane, ms, task = m.groups()
    pats = [r"^(Milestone|Task): %s" % re.escape(ident) + (r"(\.[0-9]+)?$" if not task else "$"),
            r"\b%s\b" % re.escape(ident)]
    word = r"(%s )?(milestone|step) %s" % (lane, ms)
    pats.append(word + (r",? tasks? ([0-9, and-]*\b)?%s\b" % task if task else r"\b"))
    args = ["log", "--oneline", "-E", "-i"] + ["--grep=%s" % p for p in pats]
    sys.stdout.write(git(*args))
    return 0


def cmd_cost():
    agents = os.path.join(ROOT, "AGENTS.md")
    status = os.path.join(DOCS, "STATUS.md")
    total = 0
    for p in (agents, status):
        n = len(read(p).encode())
        total += n
        print("%6d  %s" % (n, rel(p)))
    links = set(re.findall(r"\]\(([^)#\s]+\.md)", read(status)))
    sizes = sorted((len(read(os.path.join(DOCS, l)).encode()), l) for l in links
                   if os.path.exists(os.path.join(DOCS, l)))
    print("%6d  always read (about %d tokens)" % (total, total // 4))
    for n, l in sizes:
        print("%6d    + %s" % (n, l))
    return 0


def main(argv):
    if len(argv) < 2 or argv[1] in ("-h", "--help", "help"):
        print(__doc__)
        return 0
    cmd = argv[1]
    if cmd == "check":
        return run_check().show()
    if cmd == "gen":
        generate(apply=True)
        return 0
    if cmd == "find" and len(argv) == 3:
        return cmd_find(argv[2])
    if cmd == "log" and len(argv) == 3:
        return cmd_log(argv[2])
    if cmd == "cost":
        return cmd_cost()
    if cmd == "hook" and len(argv) >= 3:
        if argv[2] == "pre-commit":
            return hook_pre_commit()
        if argv[2] == "commit-msg" and len(argv) == 4:
            return hook_commit_msg(argv[3])
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))

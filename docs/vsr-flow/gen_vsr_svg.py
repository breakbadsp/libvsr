#!/usr/bin/env python3
"""Generator for vsr-flow.svg and vsr-flow.html (standard library only).

One declarative step table (STEPS below) holds the scenario. The generator
(1) resolves message and effect times, (2) runs the modelled state through
every effect with self-checks, (3) derives every keyTimes/values list from
that model, and (4) writes the animated SVG and a thin HTML player.

Usage: python3 gen_vsr_svg.py [--debug]
"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEBUG = "--debug" in sys.argv

# ----------------------------------------------------------------------------
# Geometry (user units; viewBox width 1456 = 2 x 728)
# ----------------------------------------------------------------------------
W = 1456
CL_X, CL_W, CL_H = 12, 216, 214
ALICE_Y = 196
BOB_Y = ALICE_Y + CL_H + 14
ROW_X, ROW_W, ROW_H, ROW_GAP = 570, 870, 376, 8
ROW_Y = [196 + i * (ROW_H + ROW_GAP) for i in range(3)]
ROWS_END = ROW_Y[2] + ROW_H
LANE_X = 399
CAP_Y, CAP_H = 1352, 178
LEG_Y, LEG_H = 1544, 262
NOTE_Y = 1834
H = 1862
MAP_Y = BOB_Y + CL_H + 14
MAP_H = ROWS_END - MAP_Y

CLIENTS = ["alice", "bob"]
TD, FU = "TODAY", "FUTURE"

# ----------------------------------------------------------------------------
# Colours (declared once on the root svg; dark default, light variant)
# ----------------------------------------------------------------------------
DARK = {
    "bg": "#0d1117", "fg": "#e6edf3", "muted": "#9aa7b4", "panel": "#161b22",
    "panel2": "#1c232d", "border": "#3a4557", "pill": "#222b38", "on": "#0d1117",
    "primary": "#58a6ff", "backup": "#8b98a8", "down": "#ff6b6b",
    "committed": "#4cb3ff", "uncommitted": "#e3a82b", "applied": "#3fb97c",
    "client": "#4fd1c5",
    "msg-request": "#4fd1c5", "msg-prepare": "#ff9f43", "msg-ok": "#5ad66f",
    "msg-commit": "#c58af9", "msg-viewchange": "#ff7eb6",
    "msg-recovery": "#f2d14b", "msg-report": "#7aa7ff",
    "today": "#3fcf8e", "future": "#c9a0ff", "warn": "#ffb347", "flash": "#ffe066",
}
LIGHT = {
    "bg": "#f3f5f8", "fg": "#1f2328", "muted": "#59636e", "panel": "#ffffff",
    "panel2": "#eef1f5", "border": "#b9c2cf", "pill": "#f6f8fb", "on": "#ffffff",
    "primary": "#0969da", "backup": "#6e7781", "down": "#cf222e",
    "committed": "#0a77c2", "uncommitted": "#b57600", "applied": "#1a7f4b",
    "client": "#0e8a82",
    "msg-request": "#0e8a82", "msg-prepare": "#c2570c", "msg-ok": "#1f8a35",
    "msg-commit": "#8250df", "msg-viewchange": "#bf3989",
    "msg-recovery": "#9a6700", "msg-report": "#2b5fd9",
    "today": "#1a7f4b", "future": "#8250df", "warn": "#b45309", "flash": "#c58b00",
}

RULES = """
.vsr text{fill:var(--fg);font-family:ui-sans-serif,system-ui,sans-serif;font-size:19px}
.vsr .m{font-family:ui-monospace,monospace}
.vsr .b{font-weight:700}
.vsr .u{fill:var(--muted)}
.vsr .e{text-anchor:end}
.vsr .c{text-anchor:middle}
.vsr .s20{font-size:20px}
.vsr .s22{font-size:22px}
.vsr .s24{font-size:24px}
.vsr .s28{font-size:28px}
.vsr .s30{font-size:30px}
.vsr .on{fill:var(--on)}
.vsr .bg{fill:var(--bg)}
.vsr .pn{fill:var(--panel);stroke:var(--border);stroke-width:2}
.vsr .bx{fill:var(--panel2);stroke:var(--border);stroke-width:1.5}
.vsr .ce{fill:none;stroke:var(--border);stroke-width:1.5;stroke-dasharray:6 5}
.vsr .cu{fill:var(--uncommitted);fill-opacity:.22;stroke:var(--uncommitted);stroke-width:2.5}
.vsr .cc{fill:var(--committed);fill-opacity:.22;stroke:var(--committed);stroke-width:2.5}
.vsr .ca{fill:var(--applied);fill-opacity:.24;stroke:var(--applied);stroke-width:2.5}
.vsr .ro{fill:none;stroke-width:4}
.vsr .r-pr{stroke:var(--primary)}
.vsr .r-bk{stroke:var(--backup)}
.vsr .r-vc{stroke:var(--warn)}
.vsr .r-dn{stroke:var(--down)}
.vsr .r-rc{stroke:var(--msg-recovery)}
.vsr .rb-pr{fill:var(--primary)}
.vsr .rb-bk{fill:var(--backup)}
.vsr .rb-vc{fill:var(--warn)}
.vsr .rb-dn{fill:var(--down)}
.vsr .rb-rc{fill:var(--msg-recovery)}
.vsr .dc{fill:var(--bg);fill-opacity:.6}
.vsr .fl{fill:none;stroke:var(--flash);stroke-width:4}
.vsr .fo{fill:none;stroke:var(--fg);stroke-width:2.5;stroke-dasharray:7 5}
.vsr .k-request{--c:var(--msg-request)}
.vsr .k-prepare{--c:var(--msg-prepare)}
.vsr .k-ok{--c:var(--msg-ok)}
.vsr .k-commit{--c:var(--msg-commit)}
.vsr .k-viewchange{--c:var(--msg-viewchange)}
.vsr .k-recovery{--c:var(--msg-recovery)}
.vsr .k-report{--c:var(--msg-report)}
.vsr .pill{fill:var(--pill);stroke:var(--c);stroke-width:2.5}
.vsr .pbar{fill:var(--c)}
.vsr .ln{stroke:var(--c);stroke-width:3;stroke-dasharray:8 6;fill:none;opacity:.85}
.vsr .ar{fill:var(--c)}
.vsr .sw{fill:var(--c)}
.vsr .mk{fill:var(--down)}
.vsr .lane{stroke:var(--border);stroke-width:2;stroke-dasharray:3 7;fill:none}
.vsr .chT{fill:var(--today)}
.vsr .chF{fill:none;stroke:var(--future);stroke-width:2.5;stroke-dasharray:6 4}
.vsr .pp{fill:var(--panel2);stroke:var(--border);stroke-width:1.5}
.vsr .pl{fill:var(--primary);fill-opacity:.34;stroke:var(--primary);stroke-width:2.5}
.vsr .sg{fill:var(--panel2)}
.vsr .sg2{fill:var(--border)}
.vsr .pos{fill:var(--flash)}
.vsr .ring{fill:var(--primary);stroke:none}
.vsr .dot{fill:var(--panel2);stroke:var(--border);stroke-width:1.5}
"""

# ----------------------------------------------------------------------------
# Text width estimate (DejaVu Sans metrics = wide, so the estimate is safe)
# ----------------------------------------------------------------------------
_W = {" ": .318, ".": .318, ",": .318, ":": .337, ";": .337, "-": .361, "(": .39, ")": .39,
      "[": .39, "]": .39, "{": .636, "}": .636, "=": .838, "+": .838, "|": .337, "#": .838,
      "@": 1.0, "/": .337, ">": .838, "<": .838, "→": .838, "·": .318, "—": 1.0, "⟨": .39,
      "⟩": .39, "×": .838, "_": .5, "'": .28, "?": .531, "✕": .838, "≤": .838, "↔": 1.0,
      "‹": .4, "›": .4, "▸": .7, "…": 1.0, "0": .636}
for c, w in zip("abcdefghijklmnopqrstuvwxyz",
                [.613, .635, .55, .635, .615, .352, .635, .634, .278, .278, .579, .278, .974,
                 .634, .612, .635, .635, .411, .521, .392, .634, .592, .818, .592, .592, .525]):
    _W[c] = w
for c, w in zip("ABCDEFGHIJKLMNOPQRSTUVWXYZ",
                [.684, .686, .698, .77, .632, .575, .775, .752, .295, .295, .665, .557, .863,
                 .748, .787, .603, .787, .695, .635, .611, .732, .684, .989, .684, .611, .725]):
    _W[c] = w
for c in "123456789":
    _W[c] = .636

MONO_W = 0.602


def tw(s, fs, bold=False, mono=False):
    if mono:
        return len(s) * MONO_W * fs
    w = sum(_W.get(c, 0.6) for c in s) * fs
    return w * (1.1 if bold else 1.0)


def wrap(s, maxw, fs, bold=False, mono=False):
    lines, cur = [], ""
    for word in s.split(" "):
        t = (cur + " " + word) if cur else word
        if tw(t, fs, bold, mono) <= maxw:
            cur = t
        else:
            assert cur, "word too long to wrap: %r" % word
            lines.append(cur)
            cur = word
            assert tw(cur, fs, bold, mono) <= maxw, "word too long: %r" % word
    lines.append(cur)
    return lines


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def fn(x, d=2):
    s = ("%.*f" % (d, x))
    if "." in s:
        s = s.rstrip("0").rstrip(".")
    return "0" if s in ("-0", "") else s


# ----------------------------------------------------------------------------
# Scenario data: commands (log entries)
# ----------------------------------------------------------------------------
def _op(who, req, eng, kind, coid, side, price, qty, txt):
    return dict(who=who, req=req, eng=eng, kind=kind, coid=coid, side=side,
                price=price, qty=qty, txt=txt)


OPS = {
    1: _op("alice", 1, 0, "New", 1, "Sell", 100, 10, "New Sell 10@100"),
    2: _op("bob", 1, 0, "New", 7, "Buy", 100, 4, "New Buy 4@100"),
    3: _op("alice", 2, 1, "New", 2, "Buy", 50, 3, "New Buy 3@50"),
    4: _op("bob", 1, 0, "New", 7, "Buy", 100, 4, "New Buy 4@100"),
    5: _op("alice", 3, 0, "Cancel", 1, None, None, None, "Cancel #1"),
    6: _op("alice", 3, 0, "Cancel", 1, None, None, None, "Cancel #1"),
    7: _op("bob", 3, 1, "New", 9, "Sell", 60, 2, "New Sell 2@60"),
    8: _op("alice", 4, 0, "New", 3, "Buy", 90, 1, "New Buy 1@90"),
}
NOPS = 8


def cell_l1(op):
    c = OPS[op]
    return "%d %s r%d →E%d" % (op, c["who"], c["req"], c["eng"])


class Eng:
    """Tiny model of mex::Engine: last applied op, client table, book."""

    def __init__(s):
        s.la = 0
        s.ct = {"alice": 0, "bob": 0}
        s.orders = []
        s.seq = 0

    def book(s):
        bids = sorted([o for o in s.orders if o["side"] == "Buy"], key=lambda o: (-o["price"], o["seq"]))
        asks = sorted([o for o in s.orders if o["side"] == "Sell"], key=lambda o: (o["price"], o["seq"]))
        parts = ["bid %d %s#%d q%d" % (o["price"], o["who"], o["coid"], o["qty"]) for o in bids]
        parts += ["ask %d %s#%d q%d" % (o["price"], o["who"], o["coid"], o["qty"]) for o in asks]
        return " | ".join(parts) if parts else "book empty"

    def apply(s, op):
        assert op > s.la, "Engine::Apply precondition op_number > LastAppliedOp"
        c = OPS[op]
        s.la = op
        if c["req"] <= s.ct[c["who"]]:
            return "dup"
        s.ct[c["who"]] = c["req"]
        if c["kind"] == "New":
            rem = c["qty"]
            opp = "Sell" if c["side"] == "Buy" else "Buy"
            while rem > 0:
                cands = [o for o in s.orders if o["side"] == opp and
                         ((c["price"] >= o["price"]) if c["side"] == "Buy" else (c["price"] <= o["price"]))]
                if not cands:
                    break
                best = min(cands, key=lambda o: ((o["price"] if opp == "Sell" else -o["price"]), o["seq"]))
                f = min(rem, best["qty"])
                best["qty"] -= f
                rem -= f
                if best["qty"] == 0:
                    s.orders.remove(best)
            if rem > 0:
                s.seq += 1
                s.orders.append(dict(side=c["side"], price=c["price"], qty=rem, who=c["who"],
                                     coid=c["coid"], seq=s.seq))
            return "new"
        s.orders = [o for o in s.orders if not (o["coid"] == c["coid"] and o["who"] == c["who"])]
        return "cancel"

    def key(s):
        return (s.la, s.ct["alice"], s.ct["bob"], s.book())


# ----------------------------------------------------------------------------
# World: modelled state with a timeline for every field
# ----------------------------------------------------------------------------
ROLES = ["PRIMARY", "BACKUP", "VIEW CHANGE", "DOWN", "RECOVERING"]
ROLE_ID = {"PRIMARY": "pr", "BACKUP": "bk", "VIEW CHANGE": "vc", "DOWN": "dn", "RECOVERING": "rc"}
GATEWAY = {"PRIMARY": "gateway: accepts clients", "BACKUP": "gateway: closed",
           "VIEW CHANGE": "gateway: closed", "RECOVERING": "gateway: closed",
           "DOWN": "gateway: down"}
DASH = "—"


class World:
    def __init__(s):
        s.cur, s.tl, s.changes = {}, {}, []
        s.eng = [[Eng(), Eng()] for _ in range(3)]
        s.dnames = [{}, {}]
        s.sessions = [[], [], []]
        s.frames = {}
        s.t = 0.0
        for i in range(3):
            s.set(("role", i), "PRIMARY" if i == 0 else "BACKUP")
            s.set(("status", i), "normal")
            s.set(("v", i), 0)
            s.set(("n", i), 0)
            s.set(("k", i), 0)
            for op in range(1, NOPS + 1):
                s.set(("log", i, op), "abs")
            for e in range(2):
                s.sync_eng(i, e)
        s.sessions[0] = ["alice", "bob"]
        s.sync_sess(0)
        s.sync_sess(1)
        s.sync_sess(2)
        for c in CLIENTS:
            s.set(("conn", c), "R0")
            s.set(("nxt", c), 2)
            s.set(("l1", c), "LogonAck")
            s.set(("l2", c), "seq 1")
            s.set(("l3", c), "")
            s.frames[(c, "R0")] = [("LogonAck", 1, "")]

    # -- primitive
    def set(s, key, val):
        if key in s.cur and s.cur[key] == val:
            return
        s.cur[key] = val
        lst = s.tl.setdefault(key, [])
        if lst and abs(lst[-1][0] - s.t) < 1e-9:
            lst[-1] = (s.t, val)
        else:
            lst.append((s.t, val))
        if s.t > 0:
            s.changes.append((s.t, key))

    def dsym(s, e, key):
        d = s.dnames[e]
        if key not in d:
            d[key] = len(d)
        return "D%d·%d" % (e, d[key])

    def sync_eng(s, i, e):
        g = s.eng[i][e]
        if g is None:
            for f in ("la", "cta", "ctb", "book", "dig"):
                s.set((f, i, e), DASH)
            return
        s.set(("la", i, e), g.la)
        s.set(("cta", i, e), g.ct["alice"])
        s.set(("ctb", i, e), g.ct["bob"])
        s.set(("book", i, e), g.book())
        s.set(("dig", i, e), s.dsym(e, g.key()))

    def sync_sess(s, i):
        s.set(("sess", i), ", ".join(s.sessions[i]) if s.sessions[i] else "none")

    # -- effects
    def run(s, t, spec):
        s.t = t
        name, args = spec[0], spec[1:]
        getattr(s, "fx_" + name)(*args)
        s.check()

    def fx_set(s, key, val):
        s.set(key, val)

    def fx_append(s, i, op):
        assert s.cur[("n", i)] == op - 1, "append out of order R%d op %d" % (i, op)
        s.set(("log", i, op), "unc")
        s.set(("n", i), op)

    def fx_commit(s, i, k):
        old = s.cur[("k", i)]
        assert k >= old and k <= s.cur[("n", i)], "bad commit R%d k=%d" % (i, k)
        for op in range(old + 1, k + 1):
            assert s.cur[("log", i, op)] == "unc", "commit of a missing entry R%d op %d" % (i, op)
            s.set(("log", i, op), "com")
        s.set(("k", i), k)

    def fx_apply(s, i, op):
        k = s.cur[("k", i)]
        assert isinstance(k, int) and k >= op, "apply before commit R%d op %d (k=%s)" % (i, op, k)
        assert s.cur[("log", i, op)] == "com", "apply needs a committed cell R%d op %d" % (i, op)
        e = OPS[op]["eng"]
        s.eng[i][e].apply(op)
        s.sync_eng(i, e)
        s.set(("log", i, op), "app")

    def fx_crash(s, i):
        s.eng[i] = [None, None]
        s.sessions[i] = []
        s.set(("role", i), "DOWN")
        for f in ("status", "v", "n", "k"):
            s.set((f, i), DASH)
        s.set(("sess", i), DASH)
        for op in range(1, NOPS + 1):
            s.set(("log", i, op), "abs")
        for e in range(2):
            s.sync_eng(i, e)

    def fx_restart(s, i):
        s.eng[i] = [Eng(), Eng()]
        s.sessions[i] = []
        s.set(("role", i), "RECOVERING")
        s.set(("status", i), "recovering")
        s.set(("v", i), "?")
        s.set(("n", i), 0)
        s.set(("k", i), 0)
        s.sync_sess(i)
        for e in range(2):
            s.sync_eng(i, e)

    def fx_adopt(s, i, v, n, k):
        s.set(("v", i), v)
        s.set(("status", i), "normal")
        s.set(("role", i), "BACKUP")
        for op in range(1, n + 1):
            s.set(("log", i, op), "com" if op <= k else "unc")
        s.set(("n", i), n)
        s.set(("k", i), k)

    def fx_connect(s, c, i):
        s.sessions[i].append(c)
        s.sync_sess(i)
        s.set(("conn", c), "R%d" % i)
        s.set(("nxt", c), 1)
        s.set(("l1", c), DASH)
        s.set(("l2", c), "")
        s.set(("l3", c), "")
        s.frames[(c, "R%d" % i)] = []

    def fx_frame(s, c, kind, seq, detail):
        assert seq == s.cur[("nxt", c)], "frame seq %s expected %s for %s" % (seq, s.cur[("nxt", c)], c)
        s.set(("nxt", c), seq + 1)
        s.set(("l1", c), kind)
        s.set(("l2", c), "seq %d" % seq)
        s.set(("l3", c), detail)
        s.frames[(c, s.cur[("conn", c)])].append((kind, seq, detail))

    def check(s):
        for i in range(3):
            n, k = s.cur[("n", i)], s.cur[("k", i)]
            if isinstance(n, int) and isinstance(k, int):
                assert k <= n, "commit-number > op-number on R%d" % i
            if isinstance(n, int):
                for op in range(1, NOPS + 1):
                    present = s.cur[("log", i, op)] != "abs"
                    assert present == (op <= n), "log cells and op-number disagree on R%d op %d" % (i, op)


# ----------------------------------------------------------------------------
# Step table helpers
# ----------------------------------------------------------------------------
STEPS = []


def M(frm, to, kind, label, out="ok", at=0.6):
    return dict(frm=frm, to=to, kind=kind, label=label, out=out, at=at)


def step(sid, scene, tags, title, cap, pipe=None, fx=(), msgs=(), focus=(), hold=0.0):
    STEPS.append(dict(id=sid, scene=scene, tags=list(tags), title=title, cap=cap, pipe=pipe,
                      fx=list(fx), msgs=list(msgs), focus=list(focus), hold=hold))


def NO(r, c, s, side, p, q):
    return ("NewOrder{request_number %d, client_order_id %d, symbol_id %d, %s, Limit, price %d, qty %d}"
            % (r, c, s, side, p, q))


def CANCEL(r):
    return "Cancel{request_number %d, client_order_id 1, symbol_id 1}" % r


def PREP(v, c, n, k):
    return "PREPARE⟨v=%d, m=Command %d, n=%d, k=%d⟩" % (v, c, n, k)


def POK(v, n, i):
    return "PREPAREOK⟨v=%d, n=%d, i=%d⟩" % (v, n, i)


def COMMIT(v, k):
    return "COMMIT⟨v=%d, k=%d⟩" % (v, k)


def SVC(v, i):
    return "STARTVIEWCHANGE⟨v=%d, i=%d⟩" % (v, i)


def DVC(v, lg, vp, n, k, i):
    return "DOVIEWCHANGE⟨v=%d, l=%s, v'=%d, n=%d, k=%d, i=%d⟩" % (v, lg, vp, n, k, i)


ENG_ALL = [("eng", i, e) for i in range(3) for e in range(2)]

# ---- Scene A ---------------------------------------------------------------
step("A0", "A", [TD, FU], "Start: three replicas in view 0",
     "R0 is the primary because 0 mod 3 = 0. alice and bob are logged on at the R0 gateway, and each got "
     "LogonAck seq 1. Every engine has last_applied_op 0, client table {alice: 0, bob: 0}, and an empty book. "
     "Today mex runs one replica without VSR. This picture shows the three replicas that VSR will add.",
     pipe=("req", []), focus=[("hdr", 0)])
step("A1", "A", [TD], "alice sends NewOrder to the R0 gateway",
     "alice sends NewOrder with request_number 1 to the R0 gateway. The order sells 10 of symbol 1 at price 100. "
     "Only the gateway of the primary accepts clients.",
     pipe=("req", [0]), msgs=[M("alice", "R0", "request", NO(1, 1, 1, "Sell", 100, 10))])
step("A2", "A", [TD], "The gateway validates and routes",
     "The gateway checks the request before the log: logged on, known symbol, qty > 0, price in band. All checks "
     "pass. The shared configuration routes symbol 1 to engine 0, slot 0.",
     pipe=("req", [1]), focus=[("hdr", 0)])
step("A3", "A", [TD, FU], "R0 gives the request op-number 1",
     "R0 sets n from 0 to 1. It builds Command{op_number 1, session_id 0, symbol_slot 0, order} and appends it to "
     "log[1]. The entry is not committed. TODAY: the gateway counter last_op_ makes the op number and no log exists. "
     "FUTURE: the log position is the op number.",
     pipe=("req", [2]), fx=[(0.6, "append", 0, 1)], focus=[("cell", 0, 1), ("f", 0, "n")])
step("A4", "A", [FU], "R0 sends PREPARE to both backups",
     "R0 sends PREPARE to R1 and R2. v is the view-number, m is the request, n is the op-number, and k is the "
     "commit-number.",
     pipe=("req", [3]), msgs=[M("R0", "R1", "prepare", PREP(0, 1, 1, 0)), M("R0", "R2", "prepare", PREP(0, 1, 1, 0))])
step("A5", "A", [FU], "Backups append and send PREPAREOK",
     "Each backup checks v = its view-number (0 = 0) and n = op-number + 1 (1 = 0 + 1). Both checks pass. Each "
     "backup appends log[1], sets op-number 1, and sends PREPAREOK to R0.",
     pipe=("req", [4]), fx=[(0.6, "append", 1, 1), (0.6, "append", 2, 1)],
     msgs=[M("R1", "R0", "ok", POK(0, 1, 1), at=1.5), M("R2", "R0", "ok", POK(0, 1, 2), at=1.5)])
step("A6", "A", [FU], "R0 commits op 1",
     "The PREPAREOK of R1 arrived first. f = 1 backups reached, so op 1 (and every earlier op) is committed. k goes "
     "from 0 to 1. The PREPAREOK of R2 arrived later and changes nothing.",
     pipe=("req", [5]), fx=[(0.6, "commit", 0, 1)], focus=[("f", 0, "k"), ("cell", 0, 1)])
step("A7", "A", [TD], "Up-call: Engine0.Apply(op 1)",
     "Checks: op 1 > last_applied_op 0, and request 1 > client table[alice] 0, so client table[alice] = 1. The order "
     "rests: ask 100: alice#1 qty 10. last_applied_op = 1. The engine pushes Ack{client_order_id 1, symbol_id 1, "
     "exchange_order_id (gen 1, slot 99999), leaves 10}.",
     pipe=("req", [6]), fx=[(0.6, "apply", 0, 1)], focus=[("eng", 0, 0), ("cell", 0, 1)])
step("A8", "A", [TD], "The reply: Ack seq 2 reaches alice",
     "The Ack goes to the engine outbound queue. The R0 gateway adds seq 2 (LogonAck was seq 1) and writes it to the "
     "alice connection. This Ack is the VSR reply. R1 and R2 hold op 1 in the log, not committed, not applied.",
     pipe=("req", [7]),
     msgs=[M("R0", "alice", "report",
             "Ack seq 2 {client_order_id 1, symbol_id 1, exchange_order_id (gen 1, slot 99999), leaves 10}")],
     fx=[("arr", "frame", "alice", "Ack", 2, "client_order_id 1")])

# ---- Scene B ---------------------------------------------------------------
step("B1", "B", [TD, FU], "bob sends a buy order; R0 logs op 2",
     "bob sends NewOrder with request_number 1: buy 4 of symbol 1 at 100. The gateway validates it and routes symbol 1 "
     "to engine 0. R0 sets n = 2 and appends log[2].",
     pipe=("req", [0, 1, 2]), msgs=[M("bob", "R0", "request", NO(1, 7, 1, "Buy", 100, 4))],
     fx=[("arr+0.4", "append", 0, 2)], focus=[("cell", 0, 2)])
step("B2", "B", [FU], "PREPARE carries the commit: k = 1",
     "R0 sends PREPARE with n = 2 and k = 1. The k field tells the backups that op 1 is committed. R0 needs no "
     "separate COMMIT message because new traffic flows.",
     pipe=("req", [3]), msgs=[M("R0", "R1", "prepare", PREP(0, 2, 2, 1)), M("R0", "R2", "prepare", PREP(0, 2, 2, 1))])
step("B3", "B", [FU], "Backups append, learn k = 1, and apply op 1",
     "Each backup checks v and n = op-number + 1 (2 = 1 + 1) and appends log[2]. k = 1 says that op 1 is committed. "
     "Each backup calls Engine0.Apply(op 1) with the same checks and the same result as R0. It discards the report "
     "(the report sink is FUTURE). Then k = 1.",
     pipe=("req", [3, 5, 6]),
     fx=[(0.6, "append", 1, 2), (0.6, "append", 2, 2), (1.3, "commit", 1, 1), (1.3, "commit", 2, 1),
         (2.7, "apply", 1, 1), (2.7, "apply", 2, 1)],
     focus=[("eng", 1, 0), ("eng", 2, 0)])
step("B3b", "B", [FU], "Backups send PREPAREOK for op 2",
     "Each backup sends PREPAREOK with n = 2 to R0.",
     pipe=("req", [4]), msgs=[M("R1", "R0", "ok", POK(0, 2, 1)), M("R2", "R0", "ok", POK(0, 2, 2))])
step("B4", "B", [TD, FU], "R0 commits op 2 and applies the fill",
     "One PREPAREOK (f = 1) commits op 2, so k = 2. Engine0.Apply(op 2): op 2 > last_applied_op 1; request 1 > client "
     "table[bob] 0, so client table[bob] = 1. The book fills 4 @ 100 against alice#1. The engine pushes three reports "
     "in this order: Fill (alice, maker), Fill (bob, taker), Ack (bob).",
     pipe=("req", [5, 6]), fx=[(0.6, "commit", 0, 2), (2.0, "apply", 0, 2)], focus=[("eng", 0, 0), ("cell", 0, 2)])
step("B5", "B", [TD], "The gateway writes three reports to two sessions",
     "The gateway adds the seq of each session. alice gets Fill seq 3. bob gets Fill seq 2 and Ack seq 3. The reports of "
     "one request go to two sessions, but VSR has one reply for one client.",
     pipe=("req", [7]),
     msgs=[M("R0", "alice", "report", "Fill seq 3 {to alice: maker, client_order_id 1, price 100, qty 4, leaves 6}"),
           M("R0", "bob", "report", "Fill seq 2 {to bob: taker, client_order_id 7, price 100, qty 4, leaves 0}"),
           M("R0", "bob", "report", "Ack seq 3 {to bob: client_order_id 7, exchange_order_id 0, leaves 0}")],
     fx=[("arr0", "frame", "alice", "Fill", 3, "maker, qty 4"),
         ("arr1", "frame", "bob", "Fill", 2, "taker, qty 4"),
         ("arr2", "frame", "bob", "Ack", 3, "client_order_id 7")])

# ---- Scene C ---------------------------------------------------------------
step("C1", "C", [TD, FU], "alice sends a symbol 2 order; R0 logs op 3",
     "alice sends NewOrder with request_number 2: buy 3 of symbol 2 at 50. The gateway routes symbol 2 to engine 1, "
     "slot 0. R0 sets n = 3 and appends log[3]. Engine 1 will see op 3, not ops 1 and 2.",
     pipe=("req", [0, 1, 2]), msgs=[M("alice", "R0", "request", NO(2, 2, 2, "Buy", 50, 3))],
     fx=[("arr+0.4", "append", 0, 3)], focus=[("cell", 0, 3)])
step("C2", "C", [FU], "PREPARE for op 3 carries k = 2",
     "R0 sends PREPARE with n = 3 and k = 2. The k field carries the commit of the fill (op 2).",
     pipe=("req", [3]), msgs=[M("R0", "R1", "prepare", PREP(0, 3, 3, 2)), M("R0", "R2", "prepare", PREP(0, 3, 3, 2))])
step("C3", "C", [FU], "Backups append op 3 and apply the fill (op 2)",
     "Each backup appends log[3] and sets n = 3. k = 2 says that op 2 is committed. Each backup calls "
     "Engine0.Apply(op 2): the same fill as on R0, with the report discarded. Then k = 2.",
     pipe=("req", [3, 5, 6]),
     fx=[(0.6, "append", 1, 3), (0.6, "append", 2, 3), (1.3, "commit", 1, 2), (1.3, "commit", 2, 2),
         (2.7, "apply", 1, 2), (2.7, "apply", 2, 2)],
     focus=[("eng", 1, 0), ("eng", 2, 0)])
step("C3b", "C", [FU], "Backups send PREPAREOK for op 3",
     "Each backup sends PREPAREOK with n = 3 to R0.",
     pipe=("req", [4]), msgs=[M("R1", "R0", "ok", POK(0, 3, 1)), M("R2", "R0", "ok", POK(0, 3, 2))])
step("C4", "C", [TD, FU], "R0 commits op 3 and applies it on engine 1",
     "k = 3. Engine1.Apply(op 3): op 3 > last_applied_op(E1) 0 (a gap: ops 1 and 2 went to E0). Request 2 > client "
     "table_E1[alice] 0, so it becomes 2 (a gap: request 1 went to E0). The order rests: bid 50: alice#2 qty 3.",
     pipe=("req", [5, 6]), fx=[(0.6, "commit", 0, 3), (2.0, "apply", 0, 3)], focus=[("eng", 0, 1), ("cell", 0, 3)])
step("C4b", "C", [TD], "Ack seq 4 reaches alice",
     "The engine pushes the Ack. The gateway adds seq 4 and writes it to alice. alice has now seen seq 2, 3, and 4 on "
     "her connection, in order, with no gap.",
     pipe=("req", [7]),
     msgs=[M("R0", "alice", "report", "Ack seq 4 {client_order_id 2, symbol_id 2, leaves 3}")],
     fx=[("arr", "frame", "alice", "Ack", 4, "client_order_id 2")])
step("C5", "C", [FU], "Idle: COMMIT is also the heartbeat",
     "No new request arrives before the idle timer ends. R0 sends COMMIT with k = 3 to both backups. The same message "
     "tells the backups that R0 is alive.",
     pipe=("req", [5]), msgs=[M("R0", "R1", "commit", COMMIT(0, 3)), M("R0", "R2", "commit", COMMIT(0, 3))])
step("C6", "C", [FU], "Backups apply op 3 on engine 1",
     "Each backup learns that op 3 is committed. It calls Engine1.Apply(op 3) and sets k = 3. Only engine 1 changes. A "
     "backup sends no reply.",
     pipe=("req", [5, 6]),
     fx=[(0.6, "commit", 1, 3), (0.6, "commit", 2, 3), (1.8, "apply", 1, 3), (1.8, "apply", 2, 3)],
     focus=[("eng", 1, 1), ("eng", 2, 1)])
step("C7", "C", [TD, FU], "All replicas applied op 3: digests agree",
     "StateDigest(E0) and StateDigest(E1) are equal on R0, R1, and R2: the same state gives the same digest. The digest "
     "code exists today. Comparing digests between replicas is FUTURE.",
     pipe=("req", []), focus=ENG_ALL)

# ---- Scene D ---------------------------------------------------------------
step("D1", "D", [TD], "bob sends an order for an unknown symbol",
     "bob sends NewOrder with request_number 2 and symbol_id 42. The configuration has no symbol 42.",
     pipe=("req", [0]),
     msgs=[M("bob", "R0", "request", "NewOrder{request_number 2, client_order_id 8, symbol_id 42, ...}")])
step("D1b", "D", [TD], "The gateway rejects; no op number is used",
     "The gateway sends Reject UnknownSymbol as bob seq 4. The request takes no op number, makes no log entry, and "
     "causes no PREPARE. No engine sees request 2.",
     pipe=("req", [1, 7]),
     msgs=[M("R0", "bob", "report", "Reject seq 4 {client_order_id 8, symbol_id 42, UnknownSymbol}")],
     fx=[("arr", "frame", "bob", "Reject", 4, "UnknownSymbol")])
step("D2", "D", [TD], "bob sends the first order again",
     "bob thinks that the reply to his first order was lost. He sends it again, unchanged: request_number 1, "
     "client_order_id 7.",
     pipe=("req", [0]), msgs=[M("bob", "R0", "request", NO(1, 7, 1, "Buy", 100, 4))])
step("D3", "D", [TD, FU], "The gateway accepts; R0 logs op 4",
     "The gateway has no client table and no order state, so it accepts the order. R0 sets n = 4 and appends log[4]. "
     "The retry is a normal op in the log.",
     pipe=("req", [1, 2]), fx=[(0.8, "append", 0, 4)], focus=[("cell", 0, 4)])
step("D3b", "D", [FU], "PREPARE for op 4",
     "R0 sends PREPARE with n = 4 and k = 3.",
     pipe=("req", [3]), msgs=[M("R0", "R1", "prepare", PREP(0, 4, 4, 3)), M("R0", "R2", "prepare", PREP(0, 4, 4, 3))])
step("D3c", "D", [FU], "Backups append op 4 and send PREPAREOK",
     "Each backup checks v and n = op-number + 1 (4 = 3 + 1), appends log[4], and sends PREPAREOK. k = 3 is already "
     "applied, so nothing else changes.",
     pipe=("req", [4]), fx=[(0.6, "append", 1, 4), (0.6, "append", 2, 4)],
     msgs=[M("R1", "R0", "ok", POK(0, 4, 1), at=1.5), M("R2", "R0", "ok", POK(0, 4, 2), at=1.5)])
step("D4", "D", [TD], "Engine0.Apply(op 4): a duplicate request",
     "One PREPAREOK commits op 4, so k = 4. Apply(op 4): op 4 > last_applied_op 2, but request 1 ≤ client table[bob] 1, "
     "so the order is not executed. The book and the client table do not change. Without the request number this "
     "order would buy 4 more.",
     pipe=("req", [5, 6]), fx=[(0.6, "commit", 0, 4), (2.0, "apply", 0, 4)], focus=[("eng", 0, 0), ("cell", 0, 4)])
step("D4b", "D", [TD], "bob gets Reject DuplicateRequest",
     "The engine pushes Reject with reason DuplicateRequest; bob gets it as seq 5. The paper primary would resend the "
     "stored reply. mex decides in the engine and stores no reply.",
     pipe=("req", [7]),
     msgs=[M("R0", "bob", "report", "Reject seq 5 {client_order_id 7, symbol_id 1, DuplicateRequest}")],
     fx=[("arr", "frame", "bob", "Reject", 5, "DuplicateRequest")])

# ---- Scene E ---------------------------------------------------------------
step("E1", "E", [TD, FU], "alice sends Cancel; R0 logs op 5",
     "alice sends Cancel with request_number 3 for client_order_id 1. R0 sets n = 5 and appends log[5].",
     pipe=("req", [0, 1, 2]), msgs=[M("alice", "R0", "request", CANCEL(3))],
     fx=[("arr+0.4", "append", 0, 5)], focus=[("cell", 0, 5)])
step("E1b", "E", [FU], "R0 sends PREPARE; the message to R2 is lost",
     "R0 sends PREPARE with n = 5 and k = 4 to R1 and R2. The network loses the PREPARE to R2. R1 receives its PREPARE.",
     pipe=("req", [3]),
     msgs=[M("R0", "R1", "prepare", PREP(0, 5, 5, 4)), M("R0", "R2", "prepare", PREP(0, 5, 5, 4), out="lost")])
step("E2", "E", [FU], "R1 appends op 5 and applies op 4",
     "R1 checks v and n = op-number + 1 (5 = 4 + 1) and appends log[5]. k = 4 says that op 4 is committed. R1 calls "
     "Engine0.Apply(op 4): a duplicate reject, discarded, so last_applied_op = 4. R2 has not seen op 5.",
     pipe=("req", [3, 5, 6]),
     fx=[(0.6, "append", 1, 5), (1.3, "commit", 1, 4), (2.7, "apply", 1, 4)], focus=[("eng", 1, 0), ("cell", 1, 5)])
step("E3", "E", [FU], "R0 crashes before it reads the PREPAREOK",
     "R1 sends PREPAREOK for op 5, but R0 crashes first. The message ends at a down replica. Op 5 is not committed and "
     "alice has no reply. R0 loses all state because mex has no journal. The client connections to R0 break.",
     pipe=("req", [4]),
     fx=[(0.5, "crash", 0), (0.5, "set", ("conn", "alice"), "none"), (0.5, "set", ("conn", "bob"), "none")],
     msgs=[M("R1", "R0", "ok", POK(0, 5, 1), out="down", at=1.4)], focus=[("hdr", 0)])

# ---- Scene F ---------------------------------------------------------------
step("F1", "F", [FU], "R2 times out and starts view 1",
     "R2 hears no PREPARE and no COMMIT from R0 before its timeout. It sets v from 0 to 1 and status view-change. It "
     "sends STARTVIEWCHANGE to all other replicas. R0 is down.",
     pipe=("vc", [0, 1]),
     fx=[(0.6, "set", ("v", 2), 1), (0.6, "set", ("status", 2), "view-change"), (0.6, "set", ("role", 2), "VIEW CHANGE")],
     msgs=[M("R2", "R0", "viewchange", SVC(1, 2), out="down", at=1.4), M("R2", "R1", "viewchange", SVC(1, 2), at=1.4)],
     focus=[("f", 2, "v"), ("f", 2, "status")])
step("F2", "F", [FU], "R1 joins the view change",
     "R1 receives STARTVIEWCHANGE with v = 1 > its view-number 0. It sets v = 1 and status view-change. It sends "
     "STARTVIEWCHANGE to all other replicas.",
     pipe=("vc", [1]),
     fx=[(0.6, "set", ("v", 1), 1), (0.6, "set", ("status", 1), "view-change"), (0.6, "set", ("role", 1), "VIEW CHANGE")],
     msgs=[M("R1", "R0", "viewchange", SVC(1, 1), out="down", at=1.4), M("R1", "R2", "viewchange", SVC(1, 1), at=1.4)],
     focus=[("f", 1, "v"), ("f", 1, "status")])
step("F3", "F", [FU], "R1 sends its own DOVIEWCHANGE to itself",
     "R1 has STARTVIEWCHANGE for v = 1 from f = 1 other replica (R2). The new primary is 1 mod 3 = R1, so R1 gives "
     "DOVIEWCHANGE to itself. l is its log, and v' = 0 is the last view with status normal.",
     pipe=("vc", [2, 3]), msgs=[M("R1", "R1", "viewchange", DVC(1, "log[1..5]", 0, 5, 4, 1))])
step("F4", "F", [FU], "R2 sends DOVIEWCHANGE to the new primary",
     "R2 has STARTVIEWCHANGE from f = 1 other replica (R1). It sends DOVIEWCHANGE to the new primary R1. R2 has only "
     "log[1..4].",
     pipe=("vc", [3]), msgs=[M("R2", "R1", "viewchange", DVC(1, "log[1..4]", 0, 4, 3, 2))])
step("F5", "F", [FU], "R1 has f + 1 = 2 DOVIEWCHANGE and starts view 1",
     "Largest v' is 0 in both, so the largest n wins: R1 takes its own log[1..5]. It sets n = 5, k = max(4, 3) = 4, "
     "and status normal. R1 is now PRIMARY and its gateway accepts clients. R1 sends STARTVIEW. R1 has no committed "
     "op that it did not execute.",
     pipe=("vc", [4, 5]), fx=[(0.6, "set", ("role", 1), "PRIMARY"), (0.6, "set", ("status", 1), "normal")],
     msgs=[M("R1", "R0", "viewchange", "STARTVIEW⟨v=1, l=log[1..5], n=5, k=4⟩", out="down", at=1.4),
           M("R1", "R2", "viewchange", "STARTVIEW⟨v=1, l=log[1..5], n=5, k=4⟩", at=1.4)],
     focus=[("hdr", 1)])
step("F6", "F", [FU], "R2 takes the new log and sends PREPAREOK",
     "R2 replaces its log with l and gains op 5. It sets n = 5, v = 1, and status normal. Op 5 is not committed, so it "
     "sends PREPAREOK. Then it executes the committed op that it did not execute: Engine0.Apply(op 4), a duplicate "
     "reject, discarded. k = 4.",
     pipe=("vc", [6]),
     fx=[(0.6, "append", 2, 5), (0.6, "set", ("status", 2), "normal"), (0.6, "set", ("role", 2), "BACKUP"),
         (1.8, "commit", 2, 4), (2.9, "apply", 2, 4)],
     msgs=[M("R2", "R1", "ok", POK(1, 5, 2), at=1.2)], focus=[("cell", 2, 5), ("eng", 2, 0)])
step("F7", "F", [FU, TD], "R1 commits op 5 and applies the cancel",
     "The PREPAREOK gives f = 1, so op 5 is committed and k = 5. Engine0.Apply(op 5): request 3 > client table[alice] "
     "1, so it becomes 3. The cancel executes: alice#1 leaves the book. last_applied_op = 5.",
     pipe=("req", [5, 6]), fx=[(0.6, "commit", 1, 5), (2.0, "apply", 1, 5)], focus=[("eng", 1, 0), ("cell", 1, 5)])
step("F7b", "F", [TD, FU], "The report has no connection to go to",
     "The engine pushes CancelAck. alice has no connection to R1, so the gateway drops the report (resend of reports: "
     "FUTURE). Op 5 survives because one backup had it in its log. An op that reached no backup would be gone. The "
     "retry of alice with the same request number would then execute for the first time.",
     pipe=("req", [7]),
     msgs=[M("R1", "alice", "report", "CancelAck{client_order_id 1, symbol_id 1}", out="drop")])

# ---- Scene G ---------------------------------------------------------------
step("G1", "G", [FU], "alice logs on at the gateway of R1",
     "The alice connection to R0 broke. She connects to the R1 gateway and sends Logon. The session, not the "
     "connection, owns the orders and the request numbers. How a client finds the new primary is not designed yet. In "
     "the paper the client sends to all replicas and only the primary answers.",
     pipe=("req", [0]),
     msgs=[M("alice", "R1", "request", "Logon"), M("R1", "alice", "report", "LogonAck seq 1", at="arr+0.4")],
     fx=[("arr0", "connect", "alice", 1), ("arr1", "frame", "alice", "LogonAck", 1, "")])
step("G2", "G", [TD, FU], "alice sends the cancel again; R1 logs op 6",
     "alice sends Cancel with request_number 3 again, unchanged. The gateway of R1 accepts it. R1 sets n = 6 and "
     "appends log[6].",
     pipe=("req", [0, 1, 2]), msgs=[M("alice", "R1", "request", CANCEL(3))],
     fx=[("arr+0.4", "append", 1, 6)], focus=[("cell", 1, 6)])
step("G2b", "G", [FU], "R1 sends PREPARE for op 6",
     "R1 sends PREPARE with n = 6 and k = 5. R0 is down, so that message ends at a down replica. R2 receives its "
     "PREPARE.",
     pipe=("req", [3]),
     msgs=[M("R1", "R0", "prepare", PREP(1, 6, 6, 5), out="down"), M("R1", "R2", "prepare", PREP(1, 6, 6, 5))])
step("G3", "G", [FU], "R2 applies op 5 and sends PREPAREOK for op 6",
     "R2 appends log[6] and sets n = 6. k = 5 says that op 5 is committed. R2 calls Engine0.Apply(op 5): the cancel "
     "executes, the report is discarded, and client table[alice] = 3. Then R2 sends PREPAREOK.",
     pipe=("req", [3, 4, 5, 6]),
     fx=[(0.6, "append", 2, 6), (1.2, "commit", 2, 5), (2.3, "apply", 2, 5)],
     msgs=[M("R2", "R1", "ok", POK(1, 6, 2), at=3.2)], focus=[("eng", 2, 0)])
step("G4", "G", [TD, FU], "Engine0.Apply(op 6): the retry is a duplicate",
     "One PREPAREOK commits op 6, so k = 6. Apply(op 6): request 3 ≤ client table[alice] 3, so it is not executed. The "
     "engine pushes CancelReject with reason DuplicateRequest; alice gets it as seq 2. alice now knows that request 3 "
     "was applied before.",
     pipe=("req", [5, 6, 7]),
     fx=[(0.6, "commit", 1, 6), (2.0, "apply", 1, 6), ("arr", "frame", "alice", "CancelReject", 2, "DuplicateRequest")],
     msgs=[M("R1", "alice", "report", "CancelReject seq 2 {client_order_id 1, symbol_id 1, DuplicateRequest}", at=3.2)],
     focus=[("eng", 1, 0), ("cl", "alice")])
step("G5", "G", [FU], "Idle: COMMIT tells R2 about op 6",
     "R1 is idle. It sends COMMIT with k = 6 to the other replicas. R0 is down, so that message ends at a down "
     "replica. R2 calls Engine0.Apply(op 6): a duplicate reject, discarded. k = 6 and last_applied_op = 6.",
     pipe=("req", [5, 6]),
     msgs=[M("R1", "R0", "commit", COMMIT(1, 6), out="down"), M("R1", "R2", "commit", COMMIT(1, 6))],
     fx=[("arr1+0.4", "commit", 2, 6), ("arr1+1.4", "apply", 2, 6)], focus=[("eng", 2, 0)])

# ---- Scene H ---------------------------------------------------------------
step("H1", "H", [FU], "R0 restarts and sends RECOVERY",
     "R0 restarts with empty state: status recovering, view-number unknown, empty log. It takes no part in requests or "
     "view changes. It sends RECOVERY with a nonce x to R1 and R2.",
     pipe=("rec", [0, 1]), fx=[(0.6, "restart", 0)],
     msgs=[M("R0", "R1", "recovery", "RECOVERY⟨i=0, x=nonce⟩", at=1.5), M("R0", "R2", "recovery", "RECOVERY⟨i=0, x=nonce⟩", at=1.5)],
     focus=[("hdr", 0)])
step("H2", "H", [FU], "R1 and R2 answer with RECOVERYRESPONSE",
     "Both replicas have status normal, so both answer. Only the primary R1 fills l, n, and k. R2 sends nil in those "
     "fields.",
     pipe=("rec", [2]),
     msgs=[M("R1", "R0", "recovery", "RECOVERYRESPONSE⟨v=1, x, l=log[1..6], n=6, k=6, j=1⟩"),
           M("R2", "R0", "recovery", "RECOVERYRESPONSE⟨v=1, x, l=nil, n=nil, k=nil, j=2⟩")])
step("H3", "H", [FU], "R0 takes the state of the primary",
     "R0 has f + 1 = 2 answers with its nonce, and one comes from the primary of the latest view (R1, v = 1). It sets "
     "v = 1, log = l, n = 6, k = 6, and status normal. R0 is now a BACKUP.",
     pipe=("rec", [3]), fx=[(0.8, "adopt", 0, 1, 6, 6)], focus=[("hdr", 0), ("f", 0, "v"), ("f", 0, "k")])
step("H4", "H", [FU, TD], "R0 replays the log into its engines",
     "R0 builds its engine state again: Apply(op 1) to Apply(op 6) in order, each on its engine (ops 1, 2, 4, 5, 6 on "
     "E0; op 3 on E1). Reports are discarded. With a snapshot, R0 would load it, check its StateDigest, and apply only "
     "the later ops. Snapshot and restore: FUTURE.",
     pipe=("rec", [4]),
     fx=[(0.8 + 1.1 * j, "apply", 0, op) for j, op in enumerate(range(1, 7))],
     focus=[("eng", 0, 0), ("eng", 0, 1)])
step("H5", "H", [TD, FU], "R0 digests equal the digests of R1 and R2",
     "StateDigest(E0) and StateDigest(E1) on R0 equal the values on R1 and R2. R0 has the same state again.",
     pipe=("rec", [5]), focus=ENG_ALL)

# ---- Scene I ---------------------------------------------------------------
step("I1", "I", [FU], "bob logs on at the gateway of R1",
     "bob connects to the R1 gateway and sends Logon. R1 answers with LogonAck seq 1.",
     pipe=("req", [0]),
     msgs=[M("bob", "R1", "request", "Logon"), M("R1", "bob", "report", "LogonAck seq 1", at="arr+0.4")],
     fx=[("arr0", "connect", "bob", 1), ("arr1", "frame", "bob", "LogonAck", 1, "")])
step("I1b", "I", [TD, FU], "bob sends a sell order; R1 logs op 7",
     "bob sends NewOrder with request_number 3: sell 2 of symbol 2 at 60. Request 2 was a gateway reject, so no engine "
     "saw it. R1 sets n = 7 and appends log[7].",
     pipe=("req", [0, 1, 2]), msgs=[M("bob", "R1", "request", NO(3, 9, 2, "Sell", 60, 2))],
     fx=[("arr+0.4", "append", 1, 7)], focus=[("cell", 1, 7)])
step("I1c", "I", [FU], "R1 sends PREPARE for op 7; the message to R0 is lost",
     "R1 sends PREPARE with n = 7 and k = 6 to R0 and R2. R2 receives it. The PREPARE to R0 is lost.",
     pipe=("req", [3]),
     msgs=[M("R1", "R0", "prepare", PREP(1, 7, 7, 6), out="lost"), M("R1", "R2", "prepare", PREP(1, 7, 7, 6))])
step("I2", "I", [FU], "R2 appends op 7 and sends PREPAREOK",
     "R2 checks v and n = op-number + 1 (7 = 6 + 1), appends log[7], and sends PREPAREOK.",
     pipe=("req", [3, 4]), fx=[(0.6, "append", 2, 7)], msgs=[M("R2", "R1", "ok", POK(1, 7, 2), at=1.4)])
step("I2b", "I", [TD, FU], "R1 commits op 7 and applies it on engine 1",
     "k = 7. Engine1.Apply(op 7): op 7 > last_applied_op(E1) 3. Request 3 > client table_E1[bob] 0, so it becomes 3. The "
     "order rests: ask 60: bob#9 qty 2. The Ack goes to bob as seq 2.",
     pipe=("req", [5, 6, 7]),
     fx=[(0.6, "commit", 1, 7), (2.0, "apply", 1, 7), ("arr", "frame", "bob", "Ack", 2, "client_order_id 9")],
     msgs=[M("R1", "bob", "report", "Ack seq 2 {client_order_id 9, symbol_id 2, leaves 2}", at=3.2)],
     focus=[("eng", 1, 1), ("cl", "bob")])
step("I3", "I", [TD, FU], "alice sends a buy order; R1 logs op 8",
     "alice sends NewOrder with request_number 4: buy 1 of symbol 1 at 90. R1 sets n = 8 and appends log[8].",
     pipe=("req", [0, 1, 2]), msgs=[M("alice", "R1", "request", NO(4, 3, 1, "Buy", 90, 1))],
     fx=[("arr+0.4", "append", 1, 8)], focus=[("cell", 1, 8)])
step("I3b", "I", [FU], "R1 sends PREPARE for op 8",
     "R1 sends PREPARE with n = 8 and k = 7 to R0 and R2. Both receive it.",
     pipe=("req", [3]), msgs=[M("R1", "R0", "prepare", PREP(1, 8, 8, 7)), M("R1", "R2", "prepare", PREP(1, 8, 8, 7))])
step("I4", "I", [FU], "R2 applies op 7 and sends PREPAREOK for op 8",
     "R2 appends log[8] and sets n = 8. k = 7 says that op 7 is committed. R2 calls Engine1.Apply(op 7). Then R2 sends "
     "PREPAREOK.",
     pipe=("req", [3, 4, 5, 6]),
     fx=[(0.6, "append", 2, 8), (1.2, "commit", 2, 7), (2.3, "apply", 2, 7)],
     msgs=[M("R2", "R1", "ok", POK(1, 8, 2), at=3.2)], focus=[("eng", 2, 1)])
step("I5", "I", [TD, FU], "R1 commits op 8 and applies it on engine 0",
     "The PREPAREOK of R2 gives f = 1, so op 8 is committed and k = 8. R1 does not wait for R0. Engine0.Apply(op 8): "
     "request 4 > client table_E0[alice] 3, so it becomes 4. The order rests: bid 90: alice#3 qty 1. "
     "last_applied_op(E0) = 8. The Ack goes to alice as seq 3.",
     pipe=("req", [5, 6, 7]),
     fx=[(0.6, "commit", 1, 8), (2.0, "apply", 1, 8), ("arr", "frame", "alice", "Ack", 3, "client_order_id 3")],
     msgs=[M("R1", "alice", "report", "Ack seq 3 {client_order_id 3, symbol_id 1, leaves 1}", at=3.2)],
     focus=[("eng", 1, 0), ("cl", "alice")])
step("I6", "I", [FU], "R0 sees a gap and sends GETSTATE",
     "R0 also received the PREPARE for op 8. It checks n = op-number + 1: 8 is not 6 + 1. R0 misses op 7, so it does "
     "not accept the PREPARE. It sends GETSTATE "
     "with its own op-number n' = 6 to R2.",
     pipe=("st", [0, 1]), msgs=[M("R0", "R2", "recovery", "GETSTATE⟨v=1, n'=6, i=0⟩")], focus=[("f", 0, "n")])
step("I7", "I", [FU], "R2 answers with NEWSTATE",
     "R2 has status normal in the same view, so it answers with its log after n' = 6: log[7..8], with n = 8 and k = 7.",
     pipe=("st", [2]), msgs=[M("R2", "R0", "recovery", "NEWSTATE⟨v=1, l=log[7..8], n=8, k=7⟩")])
step("I8", "I", [FU], "R0 catches up and sends PREPAREOK",
     "R0 appends log[7] and log[8] and sets n = 8. k = 7 says that op 7 is committed, so R0 calls Engine1.Apply(op 7). "
     "Then R0 sends PREPAREOK for op 8. Op 8 is already committed at R1, so this message changes nothing there.",
     pipe=("st", [3, 4]),
     fx=[(0.6, "append", 0, 7), (0.9, "append", 0, 8), (1.6, "commit", 0, 7), (2.7, "apply", 0, 7)],
     msgs=[M("R0", "R1", "ok", POK(1, 8, 0), at=3.5)], focus=[("cell", 0, 7), ("cell", 0, 8), ("eng", 0, 1)])
step("I9", "I", [FU], "Idle: COMMIT tells R0 and R2 about op 8",
     "R1 sends COMMIT with k = 8 to R0 and R2. Each calls Engine0.Apply(op 8) and sets k = 8.",
     pipe=("req", [5, 6]),
     msgs=[M("R1", "R0", "commit", COMMIT(1, 8)), M("R1", "R2", "commit", COMMIT(1, 8))],
     fx=[("arr0+0.4", "commit", 0, 8), ("arr0+1.4", "apply", 0, 8), ("arr1+0.4", "commit", 2, 8),
         ("arr1+1.4", "apply", 2, 8)],
     focus=[("eng", 0, 0), ("eng", 2, 0)])

# ---- Scene J ---------------------------------------------------------------
step("J1", "J", [FU], "All three replicas hold the same log and state",
     "All replicas have v = 1, status normal, n = 8, and k = 8. R1 is the PRIMARY. E0: last_applied_op 8, client table "
     "{alice: 4, bob: 1}, bid 90: alice#3 qty 1. E1: last_applied_op 7, client table {alice: 2, bob: 3}, bid 50: "
     "alice#2 qty 3 and ask 60: bob#9 qty 2.",
     pipe=("req", []), focus=[("f", i, f) for i in range(3) for f in ("v", "n", "k")])
step("J2", "J", [TD, FU], "Agreement check: the digests are equal",
     "StateDigest(E0) and StateDigest(E1) are equal on R0, R1, and R2: the replicas agree. A different digest at the "
     "same op number would show a determinism bug. FUTURE: write a snapshot at this op and cut the log before it. The "
     "loop then starts again.",
     pipe=("req", []), focus=ENG_ALL, hold=7.0)

SCENES = [
    ("A", "Normal operation: one order rests (op 1)"),
    ("B", "A fill: the commit travels in the next PREPARE (op 2)"),
    ("C", "The second engine, gaps, and the COMMIT heartbeat (op 3)"),
    ("D", "A gateway reject outside the log, and a duplicate request (op 4)"),
    ("E", "The primary fails with an uncommitted op (op 5)"),
    ("F", "View change 0 to 1"),
    ("G", "The client moves to the new primary and retries (op 6)"),
    ("H", "Recovery of R0"),
    ("I", "State transfer for a backup that missed a PREPARE (ops 7 and 8)"),
    ("J", "Agreement check"),
]
PIPES = {
    "req": ["client request", "gateway check", "log append", "PREPARE", "PREPAREOK", "commit", "Engine::Apply",
            "report and seq"],
    "vc": ["timeout", "STARTVIEWCHANGE", "f messages", "DOVIEWCHANGE", "f + 1 logs", "STARTVIEW", "catch up"],
    "rec": ["restart", "RECOVERY", "f + 1 answers", "take state", "replay Apply", "StateDigest"],
    "st": ["gap seen", "GETSTATE", "NEWSTATE", "append log", "Engine::Apply"],
}

# ----------------------------------------------------------------------------
# Time resolution
# ----------------------------------------------------------------------------
TOKRE = re.compile(r"^(start|arr|arr\d+|send\d+)([+-]\d+(?:\.\d+)?)?$")
SPEED = 280.0
LABEL_COLS = 25
PILL_MAX_W = 320


def tok(at, arr, snd):
    if isinstance(at, (int, float)):
        return float(at)
    m = TOKRE.match(at)
    assert m, "bad time token %r" % at
    b, off = m.group(1), float(m.group(2) or 0)
    if b == "start":
        v = 0.5
    elif b == "arr":
        assert arr, "arr token needs a message"
        v = max(arr)
    elif b.startswith("arr"):
        v = arr[int(b[3:])]
    else:
        v = snd[int(b[4:])]
    return v + off


def anchor_y(name):
    if name == "alice":
        return ALICE_Y + CL_H / 2
    if name == "bob":
        return BOB_Y + CL_H / 2
    return ROW_Y[int(name[1])] + ROW_H / 2


def disp(name):
    return name


def edge_x(name):
    return CL_X + CL_W if name in CLIENTS else ROW_X


def build_msg(m0):
    m = dict(m0)
    lines = wrap(m["label"], LABEL_COLS * MONO_W * 19 + 0.5, 19, mono=True)
    hdr = "%s → %s" % (m["frm"], m["to"]) if m["frm"] != m["to"] else "%s → %s (itself)" % (m["frm"], m["to"])
    m["hdr"], m["lines"] = hdr, lines
    cols = max(len(hdr), max(len(x) for x in lines))
    m["w"] = max(176.0, cols * MONO_W * 19 + 30)
    assert m["w"] <= PILL_MAX_W, "pill too wide: %s" % m["label"]
    m["h"] = (len(lines) + 1) * 22 + 16
    m["y0"], m["y1"] = anchor_y(m["frm"]), anchor_y(m["to"])
    frac = {"ok": 1.0, "down": 1.0, "lost": 0.55, "drop": 0.2}[m["out"]]
    m["ye"] = m["y0"] + (m["y1"] - m["y0"]) * frac
    dist = abs(m["ye"] - m["y0"])
    m["travel"] = 1.5 if m["frm"] == m["to"] else max(1.2, dist / SPEED)
    m["ext"] = 40 if m["out"] != "ok" else 0
    m["dwell"] = 0.5 if m["out"] == "ok" else 2.2
    return m


def y_at(m, tau):
    if tau <= m["ts"]:
        return m["y0"]
    if tau >= m["te"]:
        return m["ye"]
    return m["y0"] + (m["ye"] - m["y0"]) * (tau - m["ts"]) / (m["te"] - m["ts"])


def box_at(m, tau):
    y = y_at(m, tau)
    return (y - m["h"] / 2 - m["ext"], y + m["h"] / 2)


def conflicts(m, placed):
    tau = m["ts"]
    while tau <= m["vend"]:
        bm = box_at(m, tau)
        for p in placed:
            if p["ts"] <= tau <= p["vend"]:
                bp = box_at(p, tau)
                if bm[0] < bp[1] + 12 and bp[0] < bm[1] + 12:
                    return True
        tau += 0.05
    return False


def resolve_steps():
    t0 = 0.0
    for st in STEPS:
        placed, arr, snd = [], [], []
        for m0 in st["msgs"]:
            m = build_msg(m0)
            ts = tok(m["at"], arr, snd)
            for _ in range(400):
                m["ts"] = ts
                m["te"] = ts + m["travel"]
                m["vend"] = m["te"] + m["dwell"]
                if not conflicts(m, placed):
                    break
                ts += 0.1
            else:
                raise AssertionError("cannot place message %s" % m["label"])
            arr.append(m["te"])
            snd.append(m["ts"])
            placed.append(m)
        st["_m"] = placed
        fxr = []
        for spec in st["fx"]:
            fxr.append((tok(spec[0], arr, snd),) + tuple(spec[1:]))
        st["_fx"] = fxr
        last = max([0.0] + [m["vend"] for m in placed] + [f[0] for f in fxr])
        words = len(st["cap"].split())
        dur = max(3.5 if placed else 3.2, last + 1.4, 1.8 + words / 5.0, st["hold"])
        dur = round(dur * 20) / 20.0
        st["t0"], st["dur"] = t0, dur
        t0 += dur
    return t0


TOTAL = resolve_steps()
NSTEPS = len(STEPS)
for _i, _st in enumerate(STEPS):
    _st["n"] = _i + 1


def abs_msgs():
    out = []
    for st in STEPS:
        for m in st["_m"]:
            mm = dict(m)
            for k in ("ts", "te", "vend"):
                mm[k] = round(st["t0"] + m[k], 3)
            mm["step"] = st["n"]
            out.append(mm)
    return out


MSGS = abs_msgs()

# ----------------------------------------------------------------------------
# Run the model through every effect (self-checks inside World)
# ----------------------------------------------------------------------------
WORLD = World()


def exp_rep(w, i, **kw):
    for f, v in kw.items():
        assert w[(f, i)] == v, "R%d %s = %r, expected %r" % (i, f, w[(f, i)], v)


def exp_eng(w, i, e, la, cta, ctb, book):
    got = (w[("la", i, e)], w[("cta", i, e)], w[("ctb", i, e)], w[("book", i, e)])
    assert got == (la, cta, ctb, book), "R%d E%d = %r expected %r" % (i, e, got, (la, cta, ctb, book))


def same_digests(w):
    for e in range(2):
        d = {w[("dig", i, e)] for i in range(3)}
        assert len(d) == 1, "digests differ on E%d: %r" % (e, d)


A_BOOK1, A_BOOK6 = "ask 100 alice#1 q10", "ask 100 alice#1 q6"
BE1 = "bid 50 alice#2 q3"
BE1b = "bid 50 alice#2 q3 | ask 60 bob#9 q2"
EMPTY = "book empty"


def check_scene(sc, w):
    if sc == "A":
        exp_rep(w, 0, n=1, k=1, v=0, status="normal", role="PRIMARY")
        exp_eng(w, 0, 0, 1, 1, 0, A_BOOK1)
        exp_eng(w, 0, 1, 0, 0, 0, EMPTY)
        for i in (1, 2):
            exp_rep(w, i, n=1, k=0, v=0, status="normal", role="BACKUP")
            assert w[("log", i, 1)] == "unc"
            exp_eng(w, i, 0, 0, 0, 0, EMPTY)
    elif sc == "B":
        exp_rep(w, 0, n=2, k=2)
        exp_eng(w, 0, 0, 2, 1, 1, A_BOOK6)
        for i in (1, 2):
            exp_rep(w, i, n=2, k=1)
            exp_eng(w, i, 0, 1, 1, 0, A_BOOK1)
    elif sc == "C":
        for i in range(3):
            exp_rep(w, i, n=3, k=3)
            exp_eng(w, i, 0, 2, 1, 1, A_BOOK6)
            exp_eng(w, i, 1, 3, 2, 0, BE1)
        same_digests(w)
    elif sc == "D":
        exp_rep(w, 0, n=4, k=4)
        exp_eng(w, 0, 0, 4, 1, 1, A_BOOK6)
        for i in (1, 2):
            exp_rep(w, i, n=4, k=3)
            exp_eng(w, i, 0, 2, 1, 1, A_BOOK6)
    elif sc == "E":
        exp_rep(w, 0, role="DOWN")
        exp_rep(w, 1, v=0, status="normal", n=5, k=4)
        exp_eng(w, 1, 0, 4, 1, 1, A_BOOK6)
        exp_rep(w, 2, v=0, status="normal", n=4, k=3)
        exp_eng(w, 2, 0, 2, 1, 1, A_BOOK6)
        exp_eng(w, 2, 1, 3, 2, 0, BE1)
    elif sc == "F":
        exp_rep(w, 1, v=1, status="normal", role="PRIMARY", n=5, k=5)
        exp_eng(w, 1, 0, 5, 3, 1, EMPTY)
        exp_rep(w, 2, v=1, status="normal", n=5, k=4)
        exp_eng(w, 2, 0, 4, 1, 1, A_BOOK6)
        exp_rep(w, 0, role="DOWN")
    elif sc == "G":
        for i in (1, 2):
            exp_rep(w, i, v=1, n=6, k=6)
            exp_eng(w, i, 0, 6, 3, 1, EMPTY)
            exp_eng(w, i, 1, 3, 2, 0, BE1)
        assert w[("dig", 1, 0)] == w[("dig", 2, 0)] and w[("dig", 1, 1)] == w[("dig", 2, 1)]
    elif sc == "H":
        exp_rep(w, 0, v=1, n=6, k=6, status="normal", role="BACKUP")
        exp_eng(w, 0, 0, 6, 3, 1, EMPTY)
        exp_eng(w, 0, 1, 3, 2, 0, BE1)
        same_digests(w)
    elif sc in ("I", "J"):
        for i in range(3):
            exp_rep(w, i, v=1, n=8, k=8, status="normal", role="PRIMARY" if i == 1 else "BACKUP")
            exp_eng(w, i, 0, 8, 4, 1, "bid 90 alice#3 q1")
            exp_eng(w, i, 1, 7, 2, 3, BE1b)
            for op in range(1, 9):
                assert w[("log", i, op)] == "app"
        same_digests(w)


def run_model():
    for si, st in enumerate(STEPS):
        for spec in sorted(enumerate(st["_fx"]), key=lambda p: (p[1][0], p[0])):
            t, *rest = spec[1]
            WORLD.run(round(st["t0"] + t, 3), tuple(rest))
        nxt = STEPS[si + 1]["scene"] if si + 1 < len(STEPS) else None
        if nxt != st["scene"]:
            check_scene(st["scene"], WORLD.cur)
    f = WORLD.frames
    assert f[("alice", "R0")] == [("LogonAck", 1, ""), ("Ack", 2, "client_order_id 1"), ("Fill", 3, "maker, qty 4"),
                                  ("Ack", 4, "client_order_id 2")]
    assert [x[:2] for x in f[("bob", "R0")]] == [("LogonAck", 1), ("Fill", 2), ("Ack", 3), ("Reject", 4), ("Reject", 5)]
    assert [x[:3] for x in f[("alice", "R1")]] == [("LogonAck", 1, ""), ("CancelReject", 2, "DuplicateRequest"),
                                                   ("Ack", 3, "client_order_id 3")]
    assert [x[:2] for x in f[("bob", "R1")]] == [("LogonAck", 1), ("Ack", 2)]
    # the model never changes a field after TOTAL
    for k, lst in WORLD.tl.items():
        ts = [t for t, _ in lst]
        assert ts == sorted(ts) and ts[-1] < TOTAL, "timeline of %r is not monotonic" % (k,)
        if k[0] == "la":
            vals = [v for _, v in lst if isinstance(v, int)]
            # last_applied_op only increases, except after a crash or restart (value resets to 0)
            for a, b in zip(vals, vals[1:]):
                assert b >= a or b == 0, "last_applied_op decreased for %r" % (k,)


run_model()

# global message overlap check (every 50 ms)
def check_messages():
    t = 0.0
    while t < TOTAL:
        vis = [m for m in MSGS if m["ts"] <= t <= m["vend"]]
        boxes = sorted((box_at(dict(m, ts=m["ts"], te=m["te"]), t), m["label"]) for m in vis)
        for (a, la), (b, lb) in zip(boxes, boxes[1:]):
            assert a[1] <= b[0], "pills overlap at t=%.2f: %s / %s" % (t, la, lb)
        t += 0.05
    for st in STEPS:
        for m in st["_m"]:
            assert m["ts"] >= 0.3 and m["vend"] + 0.2 <= st["dur"], "message leaves its step: %s" % m["label"]


check_messages()

# ----------------------------------------------------------------------------
# SVG building blocks
# ----------------------------------------------------------------------------
DURS = "%ss" % fn(TOTAL, 3)
GEO = {}
DBG_COUNT = [0]


def tkey(t):
    s = "%.6f" % (t / TOTAL)
    s = s.rstrip("0").rstrip(".")
    return s if s else "0"


def anim_pts(pts, attr="display", calc="discrete"):
    """pts: list of (time, value). Builds one animate element; keyTimes strictly increasing, end at 1."""
    clean = []
    for t, v in pts:
        t = min(max(t, 0.0), TOTAL)
        if clean and abs(t - clean[-1][0]) < TOTAL * 1e-6:
            clean[-1] = (clean[-1][0], v)
        else:
            clean.append((t, v))
    assert clean[0][0] == 0.0
    if clean[-1][0] < TOTAL:
        clean.append((TOTAL, clean[-1][1]))
    keys = [tkey(t) for t, _ in clean]
    kt = [float(k) for k in keys]
    assert kt[0] == 0 and kt[-1] == 1 and all(a < b for a, b in zip(kt, kt[1:])), "bad keyTimes"
    return ('<animate attributeName="%s" calcMode="%s" dur="%s" repeatCount="indefinite" keyTimes="%s" values="%s"/>'
            % (attr, calc, DURS, ";".join(keys), ";".join(v for _, v in clean)))


def merge(ints):
    out = []
    for a, b in sorted(ints):
        if b - a < 1e-9:
            continue
        if out and a <= out[-1][1] + 1e-9:
            out[-1] = (out[-1][0], max(out[-1][1], b))
        else:
            out.append((a, b))
    return out


def disp_attrs(ints):
    """Returns (base display attribute, animate element) for visible intervals."""
    ints = merge([(max(0.0, a), min(TOTAL, b)) for a, b in ints])
    if not ints:
        return ' display="none"', ""
    if ints[0] == (0.0, TOTAL):
        return "", ""
    vis0 = ints[0][0] <= 1e-9
    pts = [(0.0, "inline" if vis0 else "none")]
    for a, b in ints:
        if a > 1e-9:
            pts.append((a, "inline"))
        if b < TOTAL - 1e-9:
            pts.append((b, "none"))
    return ("" if vis0 else ' display="none"'), anim_pts(pts)


def group(inner, ints, extra=""):
    base, an = disp_attrs(ints)
    return "<g%s%s>%s%s</g>" % (extra, base, an, inner)


def value_at(tl, t):
    last = None
    for tt, v in tl:
        if tt <= t + 1e-9:
            last = v
        else:
            break
    return last


def timeline_of(keys):
    times = sorted({0.0} | {t for k in keys for t, _ in WORLD.tl[k]})
    out = []
    for t in times:
        val = tuple(value_at(WORLD.tl[k], t) for k in keys)
        if not out or out[-1][1] != val:
            out.append((t, val))
    return out


def variants(keys, render, gk=None):
    """Render one group per distinct value; each group is visible while the field has that value."""
    tl = timeline_of(keys)
    by = {}
    order = []
    for idx, (t, val) in enumerate(tl):
        end = tl[idx + 1][0] if idx + 1 < len(tl) else TOTAL
        g = gk(val) if gk else val
        if g not in by:
            by[g] = (val, [])
            order.append(g)
        by[g][1].append((t, end))
    return "".join(group(render(by[g][0]), by[g][1]) for g in order)


def T(x, y, s, cls="", maxw=None, anchor="start", fs=19, bold=False, mono=False):
    w = tw(s, fs, bold, mono)
    if maxw is not None:
        assert w <= maxw, "text too wide (%d > %d): %r" % (w, maxw, s)
    c = []
    if mono:
        c.append("m")
    if bold:
        c.append("b")
    if anchor == "end":
        c.append("e")
    elif anchor == "middle":
        c.append("c")
    if fs != 19:
        c.append("s%d" % fs)
    if cls:
        c.append(cls)
    dbg = ""
    if DEBUG and maxw is not None:
        lo = x if anchor == "start" else (x - maxw if anchor == "end" else x - maxw / 2.0)
        dbg = ' data-b="%s,%s"' % (fn(lo), fn(lo + maxw))
    return '<text x="%s" y="%s"%s%s>%s</text>' % (
        fn(x), fn(y), (' class="%s"' % " ".join(c)) if c else "", dbg, esc(s))


def R(x, y, w, h, cls, rx=0, extra=""):
    return '<rect x="%s" y="%s" width="%s" height="%s"%s class="%s"%s/>' % (
        fn(x), fn(y), fn(w), fn(h), (' rx="%s"' % rx) if rx else "", cls, extra)


# ----------------------------------------------------------------------------
# Replica rows, clients
# ----------------------------------------------------------------------------
SB = [("v", "view-number", 14, 190), ("status", "status", 212, 240), ("n", "op-number", 460, 170),
      ("k", "commit-number", 638, 215)]
CELL_W, CELL_H = 204, 74


def render_row(i):
    X, Y = ROW_X, ROW_Y[i]
    o = [R(X, Y, ROW_W, ROW_H, "pn", 12)]
    GEO[("hdr", i)] = (X + 6, Y + 6, ROW_W - 12, 48)
    # role outline + badge
    rk = [("role", i)]
    o.append(variants(rk, lambda v: R(X, Y, ROW_W, ROW_H, "ro r-" + ROLE_ID[v[0]], 12), None))
    o.append(T(X + 16, Y + 40, "R%d" % i, fs=28, bold=True))
    o.append(variants(rk, lambda v: R(X + 70, Y + 12, 178, 34, "rb-" + ROLE_ID[v[0]], 8) +
                      T(X + 159, Y + 36, v[0], "on", 170, "middle", 20, True)))
    o.append(variants(rk, lambda v: T(X + 262, Y + 36, GATEWAY[v[0]], "", 290, fs=20), lambda v: GATEWAY[v[0]]))
    o.append(variants([("sess", i)], lambda v: T(X + 562, Y + 36, "sessions: %s" % v[0], "u", 298, fs=20)))
    # VSR state band
    for f, label, bx, bw in SB:
        o.append(R(X + bx, Y + 58, bw, 40, "bx", 6))
        GEO[("f", i, f)] = (X + bx, Y + 58, bw, 40)
        o.append(T(X + bx + 8, Y + 85, label, "u", bw - 16))
        o.append(variants([(f, i)], lambda v, bx=bx, bw=bw: T(X + bx + bw - 10, Y + 85, str(v[0]), "", 150, "end", 20, True)))
    # log cells
    for op in range(1, NOPS + 1):
        c, r = (op - 1) % 4, (op - 1) // 4
        cx, cy = X + 14 + c * (CELL_W + 8), Y + 108 + r * (CELL_H + 6)
        GEO[("cell", i, op)] = (cx, cy, CELL_W, CELL_H)
        o.append(R(cx, cy, CELL_W, CELL_H, "ce", 8))

        def rc(v, cx=cx, cy=cy, op=op):
            st = v[0]
            if st == "abs":
                return T(cx + 10, cy + 24, "%d  empty" % op, "u", CELL_W - 20, mono=True)
            cls = {"unc": "cu", "com": "cc", "app": "ca"}[st]
            word = {"unc": "uncommitted", "com": "committed", "app": "applied"}[st]
            return (R(cx, cy, CELL_W, CELL_H, cls, 8) + T(cx + 10, cy + 22, cell_l1(op), "", CELL_W - 20, mono=True, bold=True)
                    + T(cx + 10, cy + 44, OPS[op]["txt"], "", CELL_W - 20, mono=True)
                    + T(cx + 10, cy + 65, word, "u", CELL_W - 20, mono=True))
        o.append(variants([("log", i, op)], rc))
    # engines
    for e in range(2):
        ex, ey, ew, eh = X + 14 + e * 426, Y + 268, 416, 104
        GEO[("eng", i, e)] = (ex, ey, ew, eh)
        o.append(R(ex, ey, ew, eh, "bx", 8))
        o.append(T(ex + 10, ey + 23, "Engine %d · symbol %d" % (e, e + 1), "", ew - 20, bold=True, fs=20))
        o.append(variants([("la", i, e)], lambda v, ex=ex: T(ex + 10, ey + 47, "last_applied_op %s" % v[0], "", 200)))
        o.append(variants([("dig", i, e)], lambda v, ex=ex, ew=ew: T(ex + ew - 10, ey + 47, "StateDigest %s" % v[0], "", 190, "end")))
        o.append(variants([("cta", i, e), ("ctb", i, e)],
                          lambda v, ex=ex: T(ex + 10, ey + 71, "client table {alice: %s, bob: %s}" % v if v[0] != DASH
                                             else "client table %s" % DASH, "", ew - 20)))
        o.append(variants([("book", i, e)], lambda v, ex=ex: T(ex + 10, ey + 95, v[0], "", ew - 20)))
    # cover while down
    dl = [(t, e2) for t, e2 in _intervals_of([("role", i)], lambda v: v[0] == "DOWN")]
    o.append(group(R(X + 8, Y + 104, ROW_W - 16, ROW_H - 112, "dc", 8), dl))
    return "".join(o)


def _intervals_of(keys, pred):
    tl = timeline_of(keys)
    out = []
    for idx, (t, val) in enumerate(tl):
        end = tl[idx + 1][0] if idx + 1 < len(tl) else TOTAL
        if pred(val):
            out.append((t, end))
    return out


def render_client(name):
    y = ALICE_Y if name == "alice" else BOB_Y
    x = CL_X
    o = [R(x, y, CL_W, CL_H, "pn", 12)]
    GEO[("cl", name)] = (x, y, CL_W, CL_H)
    iw = CL_W - 20
    o.append(T(x + 12, y + 29, name, "", iw, bold=True, fs=22))
    o.append(T(x + 12, y + 54, "SessionId %d" % CLIENTS.index(name), "u", iw))
    o.append(variants([("conn", name)], lambda v: T(x + 12, y + 79, "connected: %s" % v[0], "", iw)))
    o.append(variants([("nxt", name)], lambda v: T(x + 12, y + 104, "next seq %s" % v[0], "", iw)))
    o.append(T(x + 12, y + 132, "last frame:", "u", iw))
    o.append(variants([("l1", name), ("l2", name), ("l3", name)],
                      lambda v: T(x + 12, y + 156, v[0], "", iw, bold=True) + T(x + 12, y + 180, v[1], "", iw)
                      + T(x + 12, y + 204, v[2], "", iw)))
    return "".join(o)


MAP_TEXT = [
    ("State machine", "mex::Engine. Up-call: Engine::Apply [TODAY]"),
    ("Log entry m", "a Command, 64 bytes, with op_number, session_id, symbol_slot, order"),
    ("Routing", "symbol 1 to E0 slot 0, symbol 2 to E1 slot 0"),
    ("TODAY", "the gateway counter last_op_ stands in for the log"),
    ("FUTURE", "log position = op number. Apply after commit."),
    ("Client table", "inside each engine, set at Apply [TODAY]"),
    ("Reports", "a backup discards them. Sink: FUTURE."),
]


def render_map():
    x, y = CL_X, MAP_Y
    o = [R(x, y, CL_W, MAP_H, "pn", 12), T(x + 12, y + 28, "mex mapping", "", CL_W - 24, bold=True, fs=22)]
    cy = y + 52
    for lead, body in MAP_TEXT:
        lines = wrap(lead + ": " + body, CL_W - 24, 19)
        for li, ln in enumerate(lines):
            if li == 0:
                o.append('<text x="%s" y="%s"><tspan class="b">%s</tspan>%s</text>' % (
                    fn(x + 12), fn(cy), esc(lead + ":"), esc(ln[len(lead) + 1:])))
            else:
                o.append(T(x + 12, cy, ln, "", CL_W - 24))
            cy += 21
        cy += 7
    assert cy - 7 <= y + MAP_H - 6, "mapping panel text does not fit (%d > %d)" % (cy, y + MAP_H)
    return "".join(o)


# ----------------------------------------------------------------------------
# Header: title, scene, chips, progress bar, pipeline
# ----------------------------------------------------------------------------
def scene_ranges():
    out = []
    for sc, name in SCENES:
        ss = [s for s in STEPS if s["scene"] == sc]
        out.append((sc, name, ss[0]["t0"], ss[-1]["t0"] + ss[-1]["dur"]))
    return out


SCENE_R = scene_ranges()
BAR_X0, BAR_W = 24, 1408


def chips(x_right, y, tagname_ints):
    o = []
    for tag, w in (("FUTURE", 112), ("TODAY", 96)):
        ints = tagname_ints[tag]
        x = x_right - w
        if tag == "TODAY":
            inner = R(x, y, w, 30, "chT", 7) + T(x + w / 2, y + 22, tag, "on", w - 8, "middle", 19, True)
        else:
            inner = R(x, y, w, 30, "chF", 7) + T(x + w / 2, y + 22, tag, "", w - 8, "middle", 19, True)
        o.append(group(inner, ints))
        x_right = x - 10
    return "".join(o)


def render_header2():
    o = [T(24, 38, "VSR around mex: events, states, data flow", "", 900, fs=30, bold=True)]
    for sc, name, a, b in SCENE_R:
        o.append(group(T(24, 76, "Scene %s — %s" % (sc, name), "u", 1130, fs=22), [(a, b)]))
    tagints = {TD: [], FU: []}
    for st in STEPS:
        for t in st["tags"]:
            tagints[t].append((st["t0"], st["t0"] + st["dur"]))
    o.append(chips(1432, 52, tagints))
    # progress bar
    o.append(R(BAR_X0, 104, BAR_W, 8, "sg", 4))
    for k, (sc, name, a, b) in enumerate(SCENE_R):
        x0 = BAR_X0 + a / TOTAL * BAR_W
        x1 = BAR_X0 + b / TOTAL * BAR_W
        o.append(R(x0 + 1, 104, max(1, x1 - x0 - 2), 8, "sg2" if k % 2 == 0 else "sg", 3))
    for sc, name, a, b in SCENE_R:
        x0 = BAR_X0 + a / TOTAL * BAR_W
        cx = x0 + 15
        o.append('<circle cx="%s" cy="108" r="15" class="dot"/>' % fn(cx))
        o.append(group('<circle cx="%s" cy="108" r="15" class="ring"/>' % fn(cx), [(a, b)]))
        o.append(group(T(cx, 115, sc, "on", 24, "middle", 19, True), [(a, b)]))
        o.append(group(T(cx, 115, sc, "", 24, "middle", 19, True), [(0, a), (b, TOTAL)]))
    o.append('<g><animateTransform attributeName="transform" type="translate" calcMode="linear" dur="%s" '
             'repeatCount="indefinite" keyTimes="0;1" values="0,0;%d,0"/>%s</g>'
             % (DURS, BAR_W, R(BAR_X0 - 2, 92, 4, 32, "pos", 2)))
    # pipelines
    pname = {}
    for st in STEPS:
        if st["pipe"] is not None:
            pname[st["id"]] = st["pipe"]
    cur = "req"
    pipe_ints = {p: [] for p in PIPES}
    lit = {}
    for st in STEPS:
        if st["pipe"] is not None:
            cur = st["pipe"][0]
        pipe_ints[cur].append((st["t0"], st["t0"] + st["dur"]))
        if st["pipe"] is not None:
            for idx in st["pipe"][1]:
                lit.setdefault((st["pipe"][0], idx), []).append((st["t0"], st["t0"] + st["dur"]))
    for p, labels in PIPES.items():
        n = len(labels)
        gap = 10
        wts = [tw(lab, 19) + 28 for lab in labels]
        scale = (1424 - gap * (n - 1)) / sum(wts)
        inner = []
        x = 16.0
        for idx, lab in enumerate(labels):
            nw = wts[idx] * scale
            inner.append(R(x, 134, nw, 38, "pp", 8))
            if (p, idx) in lit:
                inner.append(group(R(x, 134, nw, 38, "pl", 8), lit[(p, idx)]))
            inner.append(T(x + nw / 2, 159, lab, "", nw - 8, "middle", 19))
            x += nw + gap
        o.append(group("".join(inner), pipe_ints[p]))
    return "".join(o)


# ----------------------------------------------------------------------------
# Bus, messages
# ----------------------------------------------------------------------------
def render_bus():
    o = ['<line x1="%d" y1="%d" x2="%d" y2="%d" class="lane"/>' % (LANE_X, ROW_Y[0], LANE_X, ROWS_END)]
    for i in range(3):
        y = ROW_Y[i] + ROW_H / 2
        o.append('<line x1="%d" y1="%s" x2="%d" y2="%s" class="lane"/>' % (LANE_X, fn(y), ROW_X, fn(y)))
        o.append('<circle cx="%d" cy="%s" r="5" class="dot"/>' % (ROW_X - 2, fn(y)))
    for name in CLIENTS:
        y = anchor_y(name)
        o.append('<line x1="%d" y1="%s" x2="%d" y2="%s" class="lane"/>' % (CL_X + CL_W, fn(y), LANE_X, fn(y)))
        o.append('<circle cx="%d" cy="%s" r="5" class="dot"/>' % (CL_X + CL_W + 2, fn(y)))
    o.append(T(LANE_X, ROWS_END - 6, "network", "u", 150, "middle"))
    return "".join(o)


def render_msg(m):
    kc = "k-" + m["kind"]
    x = LANE_X
    ts, te, vend = m["ts"], m["te"], m["vend"]
    out = []
    lines_out, mark_out = [], []
    # path
    if m["frm"] != m["to"]:
        pts = [(edge_x(m["frm"]), m["y0"]), (x, m["y0"]), (x, m["ye"])]
        if m["out"] in ("ok", "down"):
            pts.append((edge_x(m["to"]) + (-2 if m["to"] not in CLIENTS else 2), m["ye"]))
        poly = '<polyline class="ln" points="%s"/>' % " ".join("%s,%s" % (fn(a), fn(b)) for a, b in pts)
        if m["out"] in ("ok", "down"):
            ex = edge_x(m["to"])
            if m["to"] in CLIENTS:
                tri = "%s,%s %s,%s %s,%s" % (ex + 2, fn(m["ye"]), ex + 20, fn(m["ye"] - 9), ex + 20, fn(m["ye"] + 9))
            else:
                tri = "%s,%s %s,%s %s,%s" % (ex - 1, fn(m["ye"]), ex - 19, fn(m["ye"] - 9), ex - 19, fn(m["ye"] + 9))
            poly += '<polygon class="ar" points="%s"/>' % tri
        lines_out.append(group(poly, [(ts, vend)], ' class="%s"' % kc))
    # pill
    w, h = m["w"], m["h"]
    body = [R(-w / 2, -h / 2, w, h, "pill", 9), R(-w / 2, -h / 2, 8, h, "pbar", 4)]
    ty = -h / 2 + 24
    body.append(T(-w / 2 + 18, ty, m["hdr"], "u", w - 26, mono=True))
    for ln in m["lines"]:
        ty += 22
        body.append(T(-w / 2 + 18, ty, ln, "", w - 26, mono=True))
    y0, ye = m["y0"], m["ye"]
    kts = [0.0, ts, te, TOTAL]
    vals = ["%s,%s" % (x, fn(y0)), "%s,%s" % (x, fn(y0)), "%s,%s" % (x, fn(ye)), "%s,%s" % (x, fn(ye))]
    kt = ";".join(tkey(t) for t in kts)
    if ts <= 0:
        raise AssertionError("message at t=0")
    mv = ('<g><animateTransform attributeName="transform" type="translate" calcMode="linear" dur="%s" '
          'repeatCount="indefinite" keyTimes="%s" values="%s"/>%s</g>' % (DURS, kt, ";".join(vals), "".join(body)))
    out.append(group(mv, [(ts, vend)], ' class="%s"' % kc))
    # mark
    if m["out"] != "ok":
        txt = {"lost": "✕ LOST", "down": "✕ %s DOWN" % m["to"], "drop": "✕ DROPPED"}[m["out"]]
        mw = tw(txt, 20, True) + 24
        mk = (R(w / 2 - mw, -h / 2 - 38, mw, 32, "mk", 8) +
              T(w / 2 - mw / 2, -h / 2 - 16, txt, "on", mw - 8, "middle", 20, True))
        mark_out.append(group('<g transform="translate(%s,%s)">%s</g>' % (x, fn(ye), mk), [(te, vend)]))
    return "".join(lines_out), "".join(out), "".join(mark_out)


# ----------------------------------------------------------------------------
# Flash / focus rectangles
# ----------------------------------------------------------------------------
def flash_key(k):
    n = k[0]
    if n in ("v", "status", "n", "k"):
        return ("f", k[1], n)
    if n in ("role", "sess"):
        return ("hdr", k[1])
    if n == "log":
        return ("cell", k[1], k[2])
    if n in ("la", "cta", "ctb", "book", "dig"):
        return ("eng", k[1], k[2])
    if n in ("conn", "nxt", "l1", "l2", "l3"):
        return ("cl", k[1])
    raise KeyError(k)


def render_flash():
    fl, fo = {}, {}
    for t, k in WORLD.changes:
        fl.setdefault(flash_key(k), []).append((t, min(TOTAL, t + 1.5)))
    for m in MSGS:
        ends = [(m["frm"], m["ts"])]
        if m["out"] in ("ok", "down"):
            ends.append((m["to"], m["te"]))
        for who, tt in ends:
            key = ("cl", who) if who in CLIENTS else ("hdr", int(who[1]))
            fl.setdefault(key, []).append((tt, min(TOTAL, tt + 1.2)))
    for st in STEPS:
        for key in st["focus"]:
            fo.setdefault(tuple(key), []).append((st["t0"] + 0.3, st["t0"] + st["dur"] - 0.2))
    o = []
    for key, ints in fl.items():
        x, y, w, h = GEO[key]
        o.append(group(R(x - 2, y - 2, w + 4, h + 4, "fl", 8), ints))
    for key, ints in fo.items():
        x, y, w, h = GEO[key]
        o.append(group(R(x - 4, y - 4, w + 8, h + 8, "fo", 10), ints))
    return "".join(o)


# ----------------------------------------------------------------------------
# Caption, legend, note
# ----------------------------------------------------------------------------
CAP_FS, CAP_PITCH, CAP_LINES = 20, 26, 4
CAP_TX = 36
CAP_MAXW = 1424 - 2 * 20


def render_caption():
    o = [R(16, CAP_Y, 1424, CAP_H, "pn", 12)]
    tagints = {TD: [], FU: []}
    for st in STEPS:
        for t in st["tags"]:
            tagints[t].append((st["t0"], st["t0"] + st["dur"]))
    o.append(chips(1424, CAP_Y + 10, tagints))
    for st in STEPS:
        lines = wrap(st["cap"], CAP_MAXW, CAP_FS)
        assert len(lines) <= CAP_LINES, "caption of %s has %d lines" % (st["id"], len(lines))
        head = "Step %d (%s): %s" % (st["n"], st["id"], st["title"])
        inner = [T(CAP_TX, CAP_Y + 36, head, "", 1424 - 40 - 240, fs=22, bold=True),
                 T(1432, 38, "step %d of %d" % (st["n"], NSTEPS), "", 220, "end", 24)]
        for li, ln in enumerate(lines):
            inner.append(T(CAP_TX, CAP_Y + 72 + li * CAP_PITCH, ln, "", CAP_MAXW, fs=CAP_FS))
        o.append(group("".join(inner), [(st["t0"], st["t0"] + st["dur"])]))
    return "".join(o)


def render_legend():
    y0 = LEG_Y
    o = [R(16, y0, 1424, LEG_H, "pn", 12)]
    # block 1: messages
    x = 36
    o.append(T(x, y0 + 30, "Messages", "", 400, bold=True, fs=20))
    items = [("request", "NewOrder, Cancel, Logon"), ("prepare", "PREPARE"), ("ok", "PREPAREOK"),
             ("commit", "COMMIT (also the heartbeat)"), ("viewchange", "STARTVIEWCHANGE, DOVIEWCHANGE, STARTVIEW"),
             ("recovery", "RECOVERY, RECOVERYRESPONSE, GETSTATE, NEWSTATE"),
             ("report", "reports: Ack, Fill, Reject, LogonAck")]
    yy = y0 + 54
    for kind, text in items:
        o.append('<rect x="%d" y="%d" width="30" height="14" rx="4" class="sw k-%s"/>' % (x, yy - 12, kind))
        lines = wrap(text, 490, 19)
        for ln in lines:
            o.append(T(x + 42, yy, ln, "", 490))
            yy += 21
        yy += 3
    assert yy <= y0 + LEG_H + 2, "legend block 1 too tall"
    # block 2: log cells and tags
    x = 600
    o.append(T(x, y0 + 30, "Log cells", "", 400, bold=True, fs=20))
    cells = [("ce", "absent: not in the log"), ("cu", "appended, not committed"), ("cc", "committed, not applied"),
             ("ca", "applied by the engine")]
    yy = y0 + 44
    for cls, text in cells:
        o.append(R(x, yy, 38, 22, cls, 5))
        o.append(T(x + 52, yy + 18, text, "", 340))
        yy += 28
    o.append(T(x, yy + 22, "Step tags", "", 400, bold=True, fs=20))
    yy += 32
    o.append(R(x, yy, 96, 30, "chT", 7) + T(x + 48, yy + 22, "TODAY", "on", 88, "middle", 19, True))
    o.append(T(x + 108, yy + 22, "code exists in the repository", "", 322))
    yy += 36
    o.append(R(x, yy, 112, 30, "chF", 7) + T(x + 56, yy + 22, "FUTURE", "", 104, "middle", 19, True))
    o.append(T(x + 124, yy + 22, "needs the VSR library; not built", "", 306))
    assert yy + 34 <= y0 + LEG_H, "legend block 2 too tall"
    # block 3: protocol facts
    x = 1016
    o.append(T(x, y0 + 30, "Protocol", "", 400, bold=True, fs=20))
    facts = ["f = 1, quorum = f + 1 = 2", "primary of view v = replica (v mod 3)",
             "v view-number, n op-number, k commit-number", "LA last_applied_op, CT client table",
             "q = qty in the book line", "Gold outline: just changed. Dashed: focus."]
    yy = y0 + 56
    for f in facts:
        lines = wrap(f, 410, 19)
        for ln in lines:
            o.append(T(x, yy, ln, "", 410))
            yy += 21
        yy += 3
    assert yy <= y0 + LEG_H + 4, "legend block 3 too tall (%d)" % yy
    return "".join(o)


def render_note():
    return T(24, NOTE_Y, "Protocol: Viewstamped Replication Revisited (Liskov and Cowling, 2012). mex is prepared for it; "
                         "it does not replicate yet.", "u", 1408)


# ----------------------------------------------------------------------------
# Assemble
# ----------------------------------------------------------------------------
def build_svg(standalone=True):
    css = ("svg.vsr{" + "".join("--%s:%s;" % kv for kv in DARK.items()) + "}\n"
           "@media (prefers-color-scheme: light){svg.vsr{" + "".join("--%s:%s;" % kv for kv in LIGHT.items()) + "}}"
           + RULES)
    parts = [R(0, 0, W, H, "bg"), render_header2(), render_client("alice"), render_client("bob"), render_map(),
             render_bus()]
    parts += [render_row(i) for i in range(3)]
    parts.append(render_flash())
    rm = [render_msg(m) for m in MSGS]
    parts += [a for a, b, c in rm] + [b for a, b, c in rm] + [c for a, b, c in rm]
    parts += [render_caption(), render_legend(), render_note()]
    head = '<?xml version="1.0" encoding="UTF-8"?>\n' if standalone else ""
    svg = (head + '<svg xmlns="http://www.w3.org/2000/svg" class="vsr" viewBox="0 0 %d %d" width="%d" height="%d" '
           'role="img" aria-label="VSR around mex: events, states, data flow">'
           '<title>VSR around mex: events, states, data flow</title>'
           '<style>%s</style>%s</svg>\n' % (W, H, W, H, css, "".join(parts)))
    return svg


def build_html(svg_inline):
    steps = [round(s["t0"], 3) for s in STEPS]
    durs = [s["dur"] for s in STEPS]
    scenes = [dict(id=sc, name=name, t=round(a, 3)) for sc, name, a, b in SCENE_R]
    sb = "".join('<button class="sc" data-t="%s" title="%s">%s</button>' % (fn(a, 3), esc(name), sc)
                 for sc, name, a, b in SCENE_R)
    page = """<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>VSR around mex: events, states, data flow</title>
<style>
:root{--pbg:#0d1117;--pfg:#e6edf3;--pmu:#9aa7b4;--pbar:#161b22;--pbd:#3a4557;--pac:#58a6ff}
@media (prefers-color-scheme: light){:root{--pbg:#f3f5f8;--pfg:#1f2328;--pmu:#59636e;--pbar:#ffffff;--pbd:#b9c2cf;--pac:#0969da}}
html,body{margin:0;background:var(--pbg);color:var(--pfg);font:14px ui-sans-serif,system-ui,sans-serif}
#ctl{position:sticky;top:0;z-index:5;background:var(--pbar);border-bottom:1px solid var(--pbd);padding:6px 8px;display:flex;flex-wrap:wrap;gap:6px;align-items:center}
#ctl button{font:inherit;color:var(--pfg);background:var(--pbg);border:1px solid var(--pbd);border-radius:6px;padding:4px 10px;cursor:pointer;min-width:34px}
#ctl button:hover{border-color:var(--pac)}
#ctl button.sc.on{background:var(--pac);color:var(--pbg);border-color:var(--pac)}
#scrub{flex:1 1 220px;min-width:160px;accent-color:var(--pac)}
#clock{color:var(--pmu);font-variant-numeric:tabular-nums;min-width:120px}
svg.vsr{display:block;width:100%%;height:auto}
</style></head><body>
<div id="ctl">
<button id="pp" title="Play or pause">Pause</button>
<button id="prev" title="Previous step">&#9664; Step</button>
<button id="next" title="Next step">Step &#9654;</button>
<input id="scrub" type="range" min="0" max="%(total)s" step="0.05" value="0" aria-label="Time">
<span id="clock"></span>
<span id="scs">%(sb)s</span>
</div>
%(svg)s
<script>
(function(){
var svg=document.querySelector('svg.vsr');
var T=%(total)s, ST=%(steps)s, DU=%(durs)s, SC=%(scenes)s;
var $=function(i){return document.getElementById(i)};
var playing=true,pt=0;
function norm(t){return ((t%%T)+T)%%T}
function now(){return playing?norm(svg.getCurrentTime()):pt}
function idx(t){var k=0;for(var j=0;j<ST.length;j++){if(ST[j]<=t+1e-6)k=j;else break}return k}
function upd(t){
  $('scrub').value=t;
  var k=idx(t);
  $('clock').textContent='t '+t.toFixed(1)+' s of '+T.toFixed(0)+' s | step '+(k+1)+' of '+ST.length;
  var s=0;for(var j=0;j<SC.length;j++){if(SC[j].t<=t+1e-6)s=j}
  var b=document.querySelectorAll('.sc');for(var q=0;q<b.length;q++){b[q].classList.toggle('on',q===s)}
}
function seek(t){t=norm(t);pt=t;svg.setCurrentTime(t);upd(t)}
function setPlay(p){if(!p)pt=now();playing=p;if(p)svg.unpauseAnimations();else svg.pauseAnimations();$('pp').textContent=p?'Pause':'Play'}
$('pp').onclick=function(){setPlay(!playing)};
$('next').onclick=function(){var k=idx(now());seek(ST[(k+1)%%ST.length])};
$('prev').onclick=function(){var t=now(),k=idx(t);if(t-ST[k]>1.0)seek(ST[k]);else seek(ST[(k+ST.length-1)%%ST.length])};
$('scrub').oninput=function(){seek(parseFloat(this.value))};
var sb=document.querySelectorAll('.sc');
for(var q=0;q<sb.length;q++){sb[q].onclick=function(){seek(parseFloat(this.getAttribute('data-t')))}}
function hash(){
  var h=location.hash.replace(/^#/,'');if(!h)return false;
  var p={};h.split('&').forEach(function(x){var a=x.split('=');p[a[0]]=a[1]});
  if(p.t!==undefined){setPlay(false);seek(parseFloat(p.t));return true}
  if(p.step!==undefined){var n=parseInt(p.step,10);if(n>=1&&n<=ST.length){setPlay(false);
    seek(ST[n-1]+(p.f?parseFloat(p.f)*DU[n-1]:0));return true}}
  return false}
window.addEventListener('hashchange',hash);
window.addEventListener('load',function(){requestAnimationFrame(function(){if(!playing)svg.setCurrentTime(pt)})});
function loop(){if(playing)upd(now());requestAnimationFrame(loop)}
if(!hash())upd(0);
requestAnimationFrame(loop);
})();
</script></body></html>
""" % dict(total=fn(TOTAL, 3), steps=json.dumps(steps), durs=json.dumps(durs), scenes=json.dumps(scenes),
           sb=sb, svg=svg_inline)
    return page


def main():
    svg = build_svg(True)
    with open(os.path.join(HERE, "vsr-flow.svg"), "w", encoding="utf-8") as f:
        f.write(svg)
    if DEBUG:
        with open(os.path.join(HERE, "vsr-debug.svg"), "w", encoding="utf-8") as f:
            f.write(svg)
    inline = build_svg(False)
    with open(os.path.join(HERE, "vsr-flow.html"), "w", encoding="utf-8") as f:
        f.write(build_html(inline))
    with open(os.path.join(HERE, "vsr-steps.json"), "w", encoding="utf-8") as f:
        json.dump(dict(total=TOTAL, steps=[dict(n=s["n"], id=s["id"], scene=s["scene"], t0=s["t0"], dur=s["dur"])
                                           for s in STEPS]), f, indent=0)
    print("steps=%d total=%.2fs svg=%d bytes html=%d bytes" % (
        NSTEPS, TOTAL, os.path.getsize(os.path.join(HERE, "vsr-flow.svg")),
        os.path.getsize(os.path.join(HERE, "vsr-flow.html"))))


if __name__ == "__main__":
    main()

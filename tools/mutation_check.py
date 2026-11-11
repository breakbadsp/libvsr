#!/usr/bin/env python3
"""Mutation check of the simulator.

Puts one known protocol defect at a time into the source, builds the
simulator, and runs it. The simulator must FAIL for each defect. A defect
that it does not detect is a gap in the simulator.

Usage: tools/mutation_check.py [mutation name ...]
Needs the `gcc` CMake preset to be configured. It restores each file.
"""
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIMULATOR = os.path.join(ROOT, "build", "gcc", "sim", "vsr_sim")
SEED_COUNT = "300"
REPLICA = "src/replica.cpp"
JOURNAL = "src/journal.cpp"

# name -> (list of (file, text, replacement), extra simulator arguments)
MUTATIONS = {
    "join-without-view-start-log": ([(REPLICA,
        "if (status_ == Status::Joining && view_start_known_ && journal_.Head() >= view_start_op_)",
        "if (status_ == Status::Joining)")], []),
    "ack-before-sync": ([(REPLICA,
        "  header.op_ = op_synced_;\n  header.log_checksum_ = journal_.ChecksumAt(op_synced_);",
        "  header.op_ = journal_.Head();\n  header.log_checksum_ = journal_.HeadChecksum();")], []),
    "primary-self-ack-before-sync": ([(REPLICA,
        "  acked_ops_[config_.replica_] = op_synced_;",
        "  acked_ops_[config_.replica_] = journal_.Head();")], []),
    "primary-continues-after-restart": ([(REPLICA,
        "  if (log_is_current && PrimaryOf(view_) != config_.replica_)\n  {\n    status_ = Status::Normal;",
        "  if (log_is_current)\n  {\n    view_start_op_ = journal_.Head();\n    status_ = Status::Normal;")], []),
    "truncate-not-durable": ([(JOURNAL,
        "  if (const auto erased = EraseSlot(p_op + 1); !erased)\n  {\n    return erased;\n  }\n  head_op_ = p_op;",
        "  head_op_ = p_op;")], []),
    "best-log-by-length-only": ([(REPLICA,
        "    const bool better = record.log_view_ > leader.log_view_ ||\n"
        "                        (record.log_view_ == leader.log_view_ && record.op_ > leader.op_);",
        "    const bool better = record.op_ > leader.op_;")], []),
    "view-not-persisted-before-do-view-change": ([(REPLICA,
        "  view_ = p_view;\n  if (!PersistSuperblock())\n  {\n    return;\n  }\n  stats_.view_changes_ += 1;",
        "  view_ = p_view;\n  stats_.view_changes_ += 1;")], []),
    "commit-learned-while-unverified": ([(REPLICA,
        "      status_ == Status::Normal || (status_ == Status::Joining && log_verified_);",
        "      status_ == Status::Normal || status_ == Status::Joining;")], []),
    "duplicate-of-latest-request-prepared-again": ([(REPLICA,
        "    if (p_request <= session->request_)\n    {\n      return SubmitResult::Duplicate;\n    }",
        "    if (p_request < session->request_)\n    {\n      return SubmitResult::Duplicate;\n    }"),
        (REPLICA,
        "  else if (!is_registration && session != nullptr && session->request_ < entry.request_)",
        "  else if (!is_registration && session != nullptr && session->request_ <= entry.request_)")], []),
    "timestamp-from-wall-clock": ([(REPLICA,
        "std::max(realtime_ns_, timestamp_last_ns_ + 1);", "realtime_ns_;")], []),
    # Needs more clients than the client table holds: 12 clients, 8 sessions.
    "execute-without-session": ([(REPLICA,
        "  if (session == nullptr && !is_registration)\n  {\n    // Without a session",
        "  if (false && session == nullptr && !is_registration)\n  {\n    // Without a session"),
        (REPLICA,
        "  else if (!is_registration && session != nullptr && session->request_ < entry.request_)",
        "  else if (!is_registration && (session == nullptr || session->request_ < entry.request_))")],
        ["4000", "12"]),
}


def build(target):
    return subprocess.run(["cmake", "--build", "--preset", "gcc", "--target", target],
                          cwd=ROOT, capture_output=True, text=True)


def run_simulator(replica_count, extra):
    arguments = [SIMULATOR, "1", SEED_COUNT, replica_count] + extra
    try:
        run = subprocess.run(arguments, capture_output=True, text=True, timeout=900)
    except subprocess.TimeoutExpired:
        return "TIMEOUT"
    lines = run.stdout.strip().splitlines()
    if lines:
        return lines[-1]
    return "stopped with code %d: %s" % (run.returncode, run.stderr.strip()[-200:])


def check(name, edits, extra):
    sources = {}
    for path, old, new in edits:
        full = os.path.join(ROOT, path)
        text = sources.get(full) or open(full).read()
        if text.count(old) != 1:
            return "PATTERN NOT FOUND in %s" % path
        sources[full] = text.replace(old, new)
    for full in sources:
        shutil.copy2(full, full + ".orig")
    try:
        for full, text in sources.items():
            open(full, "w").write(text)
        if build("vsr_sim").returncode != 0:
            return "BUILD FAILED"
        for replica_count in ("3", "5", "1"):
            result = run_simulator(replica_count, extra)
            if not result.startswith("PASS"):
                return "DETECTED (%s replicas): %s" % (replica_count, result[:150])
        return "NOT DETECTED"
    finally:
        for full in sources:
            shutil.move(full + ".orig", full)
            # A restored file has its old time. Ninja must build it again.
            os.utime(full, None)


def main():
    selected = sys.argv[1:]
    failed = False
    for name, (edits, extra) in MUTATIONS.items():
        if selected and name not in selected:
            continue
        verdict = check(name, edits, extra)
        failed = failed or not verdict.startswith("DETECTED")
        print("%-44s %s" % (name, verdict), flush=True)
    build("all")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

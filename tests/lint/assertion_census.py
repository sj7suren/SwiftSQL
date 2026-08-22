# assertion_census.py — the assertion-census lint.
#
# WHY THIS EXISTS. Four engineers independently shipped tests here that passed
# whether or not the code worked. That is a systemic defect, not four mistakes,
# so this is a lint rather than a checklist — checklists do not survive schedule
# pressure. The charter rule it enforces is docs/CHARTER.md 第七节:
#
#     任何断言,若其为真不以"集合非空"为前提,即为空断言。
#
# WHAT IT READS, AND WHY NOT TOTALS. Every suite here prints one line per
# assertion AT THE MOMENT THAT ASSERTION EXECUTES ("  ok   <label>" /
# "  FAIL <label>", see tests/sync_stub.h). This lint records the MULTISET OF
# EXECUTED LABELS per target in a checked-in manifest and diffs it.
#
# A count would be strictly weaker, and the difference is the failure this lint
# was mandated for. The real shapes found in this repo:
#
#   1. An assertion inside a `for` over a collection. Empty collection => zero
#      assertions execute, suite green. The label never prints, so the census
#      shows `- <label>` and NAMES the claim that stopped being made.
#   2. A `bool` accumulator seeded `true` and asserted outside the loop.
#      "Vacuity wearing a flag."
#   3. A helper returning `true` on empty input (NoneStartWith() meant "NO
#      DELETE reached the target" passed against an executor that sent nothing).
#   4. AN ASSERTION VANISHED WHILE THE REPORTED TOTAL STAYED CONSTANT: one went
#      unexecuted (-1), a new guard replaced it (+1). A total-only check is
#      blind to this by construction. The multiset shows `- "old claim"` and
#      `+ "new guard"` side by side. THIS is why labels, not counts.
#
# THE SELF-CHECK IS LOAD-BEARING. The lint compares its OWN parsed label count
# against the "N checks" the suite reports about itself, and fails loudly if
# they disagree. Without it, a parser that silently stops matching — new report
# grammar, encoding change, a CJK label — would report "no drift" forever. That
# is this very defect class reappearing one level up, inside the tool built to
# prevent it. A census that cannot fail is a census that protects nothing.
#
# FOUR REPORT GRAMMARS + A DECOY. The suites do not agree on how to report:
#     == <suite>: N checks, M failed ==     sync_stub.h:82
#     == N checks, M failed ==              most suites
#     == N checks, M failures ==            mysql_pg_live_schema, QualifiedName
#     === N checks, M failures ===          sqlite_clone_sweep_test.cpp:448
#     N checks, M failures                  SyncWizardSpecTests.cpp:718 (bare)
# and three live suites print an INTERMEDIATE decoy that a naive regex eats:
#     -- running total before item 3: N checks, M failed --
# Anchoring alone does not exclude the decoy (its "before item 3:" looks like
# the suite-name prefix of grammar 1), so the decoy is excluded STRUCTURALLY:
# the final report is the LAST match in the stream, and '-'-delimited lines are
# rejected outright. Both, deliberately — a new decoy wording should not be
# able to reintroduce the bug.
#
# SKIPPED IS A STATE, NOT A ZERO. The env-gated live suites print SKIP and exit
# 0 without credentials. Recording them as "0 checks" would protect nothing
# forever after, so they baseline as a distinct SKIPPED state. Note that
# mysql_pg_live_mysqltx_test SKIPS *AND* PRINTS "== 0 checks, 0 failed ==" — a
# legitimate-looking zero — so skip detection deliberately overrides a present
# report line. The asymmetry that matters:
#   manifest RAN  + target skips => HARD FAIL (coverage silently lost; a target
#                                  that skips must not satisfy a real baseline)
#   manifest SKIP + target runs  => WARN only (coverage gained; failing here
#                                  would punish every dev who HAS credentials,
#                                  and a gate people route around is worse than
#                                  no gate)
#
# MANIFESTS ARE CHECKED IN, NOT GENERATED AT RUN TIME. A baseline regenerated
# on the fly auto-absorbs the very drift it exists to catch. One file per target
# (tests/lint/census/<target>.txt) so concurrent test additions do not conflict.
# Regenerate with --update; nobody hand-edits.
#
# USAGE
#   python tests/lint/assertion_census.py --build-dir build          # check
#   python tests/lint/assertion_census.py --build-dir build --update # rebaseline
#   ... [--target NAME]                                             # one target

import argparse
import json
import os
import re
import subprocess
import sys
from collections import Counter

# One assertion label, printed as the assertion executes.
LABEL_RE = re.compile(r"^[ \t]*(ok|FAIL)[ \t]+(.+?)[ \t]*$")

# The self-report. Leading delimiter run must be '=' or absent: this is what
# keeps the '--'-delimited "running total" decoys out. The optional
# "<prefix>:" covers grammar 1 (== suite: N checks ...).
REPORT_RE = re.compile(
    r"^[ \t]*(?P<lead>=*)[ \t]*"
    r"(?:(?P<suite>[^:=]*?):[ \t]*)?"
    r"(?P<checks>\d+)[ \t]+checks,[ \t]*"
    r"(?P<fails>\d+)[ \t]+(?:failed|failures)"
    r"[ \t]*=*[ \t]*$"
)

# Emitted by tests/negative_control.h when a proof is deliberately skipped.
UNPROVEN_MARK = "[negative-control: UNPROVEN"

STATE_RAN = "RAN"
STATE_SKIPPED = "SKIPPED"

MANIFEST_HEADER = (
    "# assertion census — generated by tests/lint/assertion_census.py --update\n"
    "# DO NOT HAND-EDIT. See the header of that script for what this protects.\n"
)


def discover_targets(ctest, build_dir, self_name):
    """Enumerate ctest targets from ctest itself.

    Deriving the list from ctest (rather than hand-listing) means a new test
    target is enrolled the moment it is registered — there is no separate list
    to forget to update, which is the failure mode this whole lint is about.
    """
    out = subprocess.run(
        [ctest, "--test-dir", build_dir, "--show-only=json-v1"],
        capture_output=True, text=True, check=True).stdout
    tests = []
    for t in json.loads(out)["tests"]:
        # Never recurse into ourselves.
        if t["name"] == self_name:
            continue
        tests.append((t["name"], t["command"]))
    return tests


def parse_output(text):
    """-> (labels, reported_checks|None, n_reports, skipped)"""
    labels, reports, skipped = [], [], False
    for line in text.splitlines():
        m = LABEL_RE.match(line)
        if m:
            labels.append(m.group(2))
            continue
        stripped = line.strip()
        if stripped.startswith("SKIP"):
            skipped = True
        r = REPORT_RE.match(line)
        # Reject '-'-delimited intermediate lines belt-and-braces; the
        # last-match rule below is the structural guarantee.
        if r and not stripped.startswith("-"):
            reports.append(int(r.group("checks")))
    reported = reports[-1] if reports else None
    return labels, reported, len(reports), skipped


def run_target(cmd, cwd, timeout):
    p = subprocess.run(cmd, capture_output=True, cwd=cwd, timeout=timeout)
    return p.stdout.decode("utf-8", "replace")


def manifest_path(census_dir, name):
    return os.path.join(census_dir, name + ".txt")


def read_manifest(path):
    if not os.path.exists(path):
        return None
    state, checks, labels, in_body = STATE_RAN, None, [], False
    with open(path, "r", encoding="utf-8") as f:
        for line in f.read().splitlines():
            if in_body:
                labels.append(line)
            elif line == "--":
                in_body = True
            elif line.startswith("state:"):
                state = line.split(":", 1)[1].strip()
            elif line.startswith("checks:"):
                checks = int(line.split(":", 1)[1].strip())
    return {"state": state, "checks": checks, "labels": labels}


def write_manifest(path, state, checks, labels):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(MANIFEST_HEADER)
        f.write("state: %s\n" % state)
        if state == STATE_RAN:
            f.write("checks: %d\n" % checks)
            f.write("--\n")
            for lb in labels:
                f.write(lb + "\n")
        else:
            f.write("# Env-gated: no credentials in this environment. This target\n"
                    "# must NOT be allowed to satisfy a RAN baseline by skipping.\n")


def diff_labels(want, got):
    """Multiset diff. Order is recorded for readability; identity is the
    multiset, so a reordering is not spuriously reported as drift."""
    cw, cg = Counter(want), Counter(got)
    removed = sorted((cw - cg).elements())
    added = sorted((cg - cw).elements())
    return removed, added


def check_target(name, cmd, census_dir, build_dir, timeout, update):
    """-> (ok, state, messages)"""
    msgs = []
    try:
        out = run_target(cmd, build_dir, timeout)
    except subprocess.TimeoutExpired:
        return False, "TIMEOUT", ["  timed out after %ss" % timeout]

    labels, reported, n_reports, skipped = parse_output(out)
    state = STATE_SKIPPED if (skipped and not reported) else STATE_RAN
    path = manifest_path(census_dir, name)

    # ---- self-check: the lint auditing its own parser ----------------------
    # Only meaningful for a target that actually ran. If this trips, believe
    # the SUITE and distrust the PARSER.
    if state == STATE_RAN:
        if reported is None:
            msgs.append("  SELF-CHECK FAILED: no report line matched. The suite's "
                        "report grammar is one this lint cannot read, so its "
                        "census is silently empty. Teach REPORT_RE the grammar.")
            return False, state, msgs
        if len(labels) != reported:
            msgs.append(
                "  SELF-CHECK FAILED: parsed %d assertion labels but the suite "
                "reports %d checks.\n"
                "    The parser and the suite disagree about what executed, so "
                "NO census result from this\n"
                "    target can be trusted — including a clean one. Either the "
                "harness does not print a\n"
                "    label per assertion (see EditorCompletionTests/"
                "EditorSqlFormatTests history), or the\n"
                "    label grammar changed." % (len(labels), reported))
            return False, state, msgs

    if update:
        write_manifest(path, state, reported or 0, labels)
        return True, state, ["  updated (%s, %d labels)" % (state, len(labels))]

    want = read_manifest(path)
    if want is None:
        msgs.append("  NO MANIFEST. Every target must carry a census. "
                    "Run with --update to create it.")
        return False, state, msgs

    # ---- state transitions -------------------------------------------------
    if want["state"] == STATE_RAN and state == STATE_SKIPPED:
        msgs.append("  BASELINE SAYS RAN, TARGET SKIPPED. %d assertions that the "
                    "manifest says must execute did not.\n"
                    "    A skipping target cannot satisfy a real baseline — that "
                    "is coverage lost silently." % len(want["labels"]))
        return False, state, msgs
    if want["state"] == STATE_SKIPPED and state == STATE_RAN:
        msgs.append("  NOTE: baselined SKIPPED but ran here (%d assertions) — "
                    "credentials present.\n"
                    "    Not a failure. Run --update only if this environment is "
                    "the reproducible one." % len(labels))
        return True, state, msgs
    if want["state"] == STATE_SKIPPED and state == STATE_SKIPPED:
        return True, state, []

    # ---- the census itself -------------------------------------------------
    removed, added = diff_labels(want["labels"], labels)
    ok = True
    if removed or added:
        ok = False
        msgs.append("  ASSERTION CENSUS DRIFT (baseline %d -> observed %d):"
                    % (len(want["labels"]), len(labels)))
        for lb in removed:
            msgs.append('    - "%s"' % lb)
        for lb in added:
            msgs.append('    + "%s"' % lb)
        if removed and added and len(removed) == len(added):
            msgs.append("    ^ The TOTAL IS UNCHANGED. A count-based check would "
                        "have passed this.\n"
                        "      A claim stopped being made and a different one "
                        "took its slot; confirm the\n"
                        "      removed claim is still proven somewhere before "
                        "running --update.")
    if want["checks"] is not None and reported != want["checks"]:
        ok = False
        msgs.append("  reported total %d != baseline %d" % (reported, want["checks"]))
    return ok, state, msgs


def main():
    ap = argparse.ArgumentParser(description="assertion-census lint")
    ap.add_argument("--build-dir", required=True)
    ap.add_argument("--census-dir", default=None)
    ap.add_argument("--ctest", default="ctest")
    ap.add_argument("--target", default=None, help="check one target only")
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--update", action="store_true")
    ap.add_argument("--self-name", default="assertion_census")
    args = ap.parse_args()

    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

    here = os.path.dirname(os.path.abspath(__file__))
    census_dir = args.census_dir or os.path.join(here, "census")
    build_dir = os.path.abspath(args.build_dir)

    targets = discover_targets(args.ctest, build_dir, args.self_name)
    if args.target:
        targets = [t for t in targets if t[0] == args.target]
        if not targets:
            print("no such target: %s" % args.target)
            return 2

    failed, unproven, skipped_n = [], [], 0
    for name, cmd in targets:
        ok, state, msgs = check_target(name, cmd, census_dir, build_dir,
                                       args.timeout, args.update)
        if state == STATE_SKIPPED:
            skipped_n += 1
        if not ok:
            failed.append(name)
            print("FAIL %s" % name)
        elif msgs:
            print("note %s" % name)
        for m in msgs:
            print(m)

    # Deliberately-skipped negative-control proofs are not failures, but they
    # must be VISIBLE — that is how skipped judgment reaches review without
    # becoming a checklist.
    for name, cmd in targets:
        mf = read_manifest(manifest_path(census_dir, name))
        if mf:
            for lb in mf["labels"]:
                if UNPROVEN_MARK in lb:
                    unproven.append((name, lb))

    print("\n== assertion census: %d targets, %d skipped (env-gated), %d failed =="
          % (len(targets), skipped_n, len(failed)))
    if unproven:
        print("-- %d negative control(s) UNPROVEN (allowed, but reviewed) --"
              % len(unproven))
        for name, lb in unproven:
            print("   %s: %s" % (name, lb))
    if failed:
        print("drifted: %s" % ", ".join(failed))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

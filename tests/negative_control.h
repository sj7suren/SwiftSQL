// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// negative_control.h — the negative-control gate.
//
// A NEGATIVE claim ("no DELETE reached the target", "zero divergences", "the
// list contains nothing invalid") is the shape that fails silently, because
// the cheapest way to satisfy it is to do nothing at all. The charter rule
// (docs/CHARTER.md 第七节) states the general form:
//
//     任何断言,若其为真不以"集合非空"为前提,即为空断言。
//
// The word "negative control" is not invented here — this repo already reaches
// for it under pressure: sqlite_clone_sweep_test.cpp:328 says "NEGATIVE CONTROL
// — proves the two cases above are not vacuous", and SyncCrossEngineCompare-
// Tests.cpp:321 / sqlite_clone_sweep_test.cpp:295 say "not vacuous" too. This
// header codifies what those comments were already doing by hand.
//
// THE MECHANISM. EXPECT_NEGATIVE(name, claim, brokenClaim) asserts `claim`, and
// then re-evaluates THE SAME PREDICATE against a deliberately broken subject
// and requires it to come out FALSE. If the predicate cannot tell a working
// subject from a broken one, it was never testing anything. Two labels are
// emitted, so the proof itself appears in the assertion census — a reviewer
// reading tests/lint/census/<target>.txt can see that the negative claim is
// backed. Omitting the proof does not compile: `brokenClaim` is a required
// macro argument.
//
// The worked example this generalizes is sync_exec_test.cpp: NoneStartWith() is
// vacuously true on an EMPTY statement log, so "NO DELETE reached the target"
// would also pass against an executor that never opened its mouth. The fix was
// to rest the negative claim on a connection that demonstrably received
// traffic. Written with this macro that pairing is structural rather than a
// comment someone has to remember to write:
//
//     EXPECT_NEGATIVE("exec(ins): NO DELETE reached the target",
//                     tgt->NoneStartWith(L"DELETE"),      // the claim
//                     empty.NoneStartWith(L"DELETE"));    // must be FALSE
//
// THE ESCAPE HATCH IS DELIBERATE. Some proofs are genuinely expensive to
// construct. EXPECT_NEGATIVE_UNPROVEN(name, claim, why) records the claim and
// prints an UNPROVEN line, and — critically — emits a DISTINCTLY NAMED label
// that lands in the manifest. Skipping is therefore never blocked, but it is
// never invisible either: it arrives as a census diff that a reviewer must
// accept, and the lint prints a roll-up of every UNPROVEN proof in the repo.
// That is how skipped judgment becomes visible without becoming a checklist.
//
// USAGE. These are MACROS, not functions, so they bind to whatever ExpectTrue
// is in scope — the 39 suites here declare their own (synctest::ExpectTrue,
// file-static ExpectTrue, mplive::ExpectTrue …) and none of them need to change
// to adopt this.
#pragma once

#include <cstdio>
#include <deque>
#include <string>

namespace negctl {

// Labels must outlive the call, and the suites' ExpectTrue takes const char*.
// A deque never invalidates references to existing elements on push_back, so
// every returned pointer stays valid for the life of the process.
inline const char* Label(const char* name, const char* suffix)
{
    static std::deque<std::string> pool;
    pool.emplace_back(std::string(name) + suffix);
    return pool.back().c_str();
}

// ASCII only, on purpose: these strings are compared byte-for-byte by the lint
// and printed to a Windows console that is not UTF-8.
inline const char* ProvenLabel(const char* name)
{
    return Label(name, "  [negative-control: claim is FALSE on a broken subject]");
}

inline const char* UnprovenLabel(const char* name, const char* why)
{
    static std::deque<std::string> pool;
    pool.emplace_back(std::string("  [negative-control: UNPROVEN: ") + why + "]");
    return Label(name, pool.back().c_str());
}

}  // namespace negctl

// Assert `claim`, then prove the predicate is capable of failing.
// `brokenClaim` must be the SAME predicate applied to a broken subject.
#define EXPECT_NEGATIVE(name, claim, brokenClaim)                              \
    do {                                                                       \
        ExpectTrue((name), (claim));                                           \
        ExpectTrue(negctl::ProvenLabel(name), !(brokenClaim));                 \
    } while (0)

// Documented escape. Not blocked, but it lands as a manifest diff in review.
#define EXPECT_NEGATIVE_UNPROVEN(name, claim, why)                             \
    do {                                                                       \
        ExpectTrue((name), (claim));                                           \
        std::printf("  UNPROVEN %s (%s)\n", (name), (why));                    \
        ExpectTrue(negctl::UnprovenLabel((name), (why)), true);                \
    } while (0)

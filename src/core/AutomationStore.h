// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// AutomationStore.h — the persisted "automation job" library (Navicat-style
// Automation). Each job is a sync-structure / sync-data / sync-both / export task
// bound to a source connection+database (and, for sync jobs, a target
// connection+database), optionally driven by an in-app schedule (every-N-minutes
// or daily-at-HH:MM). Jobs persist to a single INI under the user data dir:
//
//   <UserDataDir>/automation/automation.ini    — one [group] per sanitized name
//
// The store is a pure model+persistence layer (no GUI, no db drivers): the UI
// (AutomationPanel / AutomationJobDialog) reads/writes AutomationJob values and
// MainFrame's runner resolves the named connections against the live tree at
// execution time. Timestamps persist as Unix time_t (0 = never / invalid).
#pragma once

#include <wx/string.h>
#include <wx/datetime.h>
#include <vector>

namespace core {

// What a job does. The three sync flavours reuse SyncEngine; Export dumps the
// source database's schema (+ optional data) to a .sql file.
enum class JobType { SyncStructure = 0, SyncData = 1, SyncBoth = 2, Export = 3 };

// In-app scheduling only (MVP). OS-level scheduled tasks (running with the app
// closed) are a documented phase-2 item and intentionally not modelled here.
enum class ScheduleKind { None = 0, EveryMinutes = 1, DailyAt = 2 };

// Outcome of the most recent run (drives the 上次运行 status mark).
enum class RunResult { Never = 0, Success = 1, Failure = 2 };

struct AutomationJob {
    wxString     name;                     // display name (index key after Sanitize)
    JobType      type = JobType::SyncBoth;

    wxString     srcConn;                  // source connection name (ConnectionProfile::name)
    wxString     srcDb;                    // source database
    wxString     tgtConn;                  // target connection name (sync jobs only)
    wxString     tgtDb;                    // target database (sync jobs only)

    bool         dropMissing = false;      // sync: DROP target-only tables
    bool         useTransaction = true;    // sync: wrap apply in a transaction
    bool         withData = true;          // export: include INSERT rows

    ScheduleKind schedule = ScheduleKind::None;
    int          intervalMinutes = 60;     // EveryMinutes: run every N minutes
    int          dailyHour = 3;            // DailyAt: HH (0–23)
    int          dailyMinute = 0;          // DailyAt: MM (0–59)

    wxDateTime   lastRun;                  // last execution start (invalid = never)
    RunResult    lastResult = RunResult::Never;
    wxString     lastMessage;              // short outcome text (success summary / error)

    // Next-due wall-clock time given `from`, or an invalid wxDateTime when the job
    // is unscheduled. Used both by the panel (计划 column) and the timer tick.
    wxDateTime NextRunAfter(const wxDateTime& from) const;
};

class AutomationStore {
public:
    // Absolute automation directory, created (recursively) if missing.
    static wxString Dir();
    // Sub-directory that headless export jobs write their .sql dumps into.
    static wxString ExportsDir();

    // Every stored job, sorted by name (case-insensitive).
    static std::vector<AutomationJob> List();

    // Look up one job by (display) name. false when it does not exist.
    static bool Get(const wxString& name, AutomationJob& out);

    // Create/overwrite the job keyed by its (sanitized) name. false + err on I/O.
    static bool Save(const AutomationJob& job, wxString& err);

    // Delete a job's group. true if it was removed (or already gone).
    static bool Remove(const wxString& name);

    // Does a job with this (display) name already exist?
    static bool Exists(const wxString& name);

    // Filename-safe key (illegal chars → '_'); empty input yields "job".
    static wxString Sanitize(const wxString& name);
};

} // namespace core

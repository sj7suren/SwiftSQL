// RowChange.cpp — see header. The builder is the whole safety story: it is the
// only code in the tree that may construct a RowChange, and it refuses to do so
// for any row containing a !MayEmit cell.
#include "db/RowChange.h"

#include "db/DbDriver.h"   // db::Dialect (full definition), QuoteIdent

namespace db::sync {

// ---------------------------------------------------------------------------
// RowChangeBuilder
// ---------------------------------------------------------------------------

bool RowChangeBuilder::Convert(const Cell& in, const ColumnBridge& bridge,
                               std::vector<Cell>& into)
{
    // Already blocked: short-circuit. We do not keep converting a row that can
    // never be emitted, and we do not pile up one Finding per remaining cell —
    // the first refusal is the actionable one.
    if (blocked_) return false;

    const ConvertedCell cc = ConvertCell(in, bridge);

    // THE GATE. Note what is NOT possible here: the caller supplied a raw source
    // cell, not a verdict. The verdict is produced inside this function, so there
    // is no argument a caller could pass to make an Unrepresentable value look
    // emittable.
    if (!MayEmit(cc.verdict)) {
        blocked_ = true;
        Finding f;
        f.table   = table_;
        f.column  = bridge.column;
        f.verdict = ToTypeVerdict(cc.verdict);
        f.reason  = rowKeyText_.IsEmpty() ? cc.reason
                                          : (L"行 " + rowKeyText_ + L"：" + cc.reason);
        findings_.push_back(std::move(f));
        return false;
    }

    if (cc.verdict == ValueVerdict::Coerced) {
        // The row as a whole is only Exact if every cell was.
        row_.verdict_ = ValueVerdict::Coerced;
        if (recordCoercions_) {
            Finding f;
            f.table   = table_;
            f.column  = bridge.column;
            f.verdict = ToTypeVerdict(cc.verdict);
            f.reason  = cc.reason;
            findings_.push_back(std::move(f));
        }
    }

    into.push_back(cc.cell);
    return true;
}

bool RowChangeBuilder::AddCell(const Cell& in, const ColumnBridge& bridge)
{
    return Convert(in, bridge, row_.values_);
}

bool RowChangeBuilder::AddKeyCell(const Cell& in, const ColumnBridge& bridge)
{
    return Convert(in, bridge, row_.key_);
}

std::optional<RowChange> RowChangeBuilder::Build()
{
    // Blocked => no RowChange exists for this row, full stop. One unconvertible
    // cell fails the entire row: a partially converted row (say, an UPDATE that
    // silently omits the column it could not convert) is worse than no row at
    // all, because it looks like it worked.
    if (blocked_) return std::nullopt;

    // Single-use. A second Build() would hand out a moved-from row.
    if (built_) return std::nullopt;
    built_ = true;

    // Update/Delete without a key would render a WHERE-less statement. That is
    // the single most destructive thing this system could emit, so it is refused
    // at the same gate rather than trusted to the renderer.
    if (row_.op_ != RowChange::Op::Insert && row_.key_.empty()) {
        blocked_ = true;
        Finding f;
        f.table   = table_;
        f.verdict = TypeVerdict::Unmappable;
        f.reason  = L"UPDATE/DELETE 行缺少主键取值，拒绝生成无 WHERE 条件的语句";
        findings_.push_back(std::move(f));
        return std::nullopt;
    }

    // The identity travels with the row. Assigned here rather than in the
    // constructor so there is exactly one string, used for both the Finding
    // prefix above and RowChange::RowKey().
    row_.rowKey_ = rowKeyText_;

    // Prior values: copied only when a consumer asked for them (the sample), and
    // only for an Update — an Insert has no "before", and a Delete's Values()
    // are empty, so a before/after pair would be meaningless there. No
    // conversion: these are target-native display cells (see RowChange.h).
    if (keepPrior_ && prior_ && row_.op_ == RowChange::Op::Update)
        row_.prior_ = *prior_;

    return std::optional<RowChange>(std::move(row_));
}

// ---------------------------------------------------------------------------
// RowChangeSet
// ---------------------------------------------------------------------------

bool RowChangeSet::Accept(RowChangeBuilder&& builder)
{
    // Decided BEFORE Build(), because Build() is what copies. A row that will
    // not be sampled must not materialize display data it has no reader for —
    // that is how a 500k-row diff stays flat.
    builder.KeepPriorValues(sample_.size() < kSampleCap);

    std::optional<RowChange> row = builder.Build();

    for (const Finding& f : builder.Findings()) findings_.push_back(f);

    if (!row) {
        // Detected as a difference, refused by the gate. Counted separately so
        // the review grid cannot show a number that quietly excludes it, and
        // latched into blocked_ so the whole table stops being executable.
        ++stat_.blocked;
        blocked_ = true;
        return false;
    }

    switch (row->Operation()) {
    case RowChange::Op::Insert: ++stat_.inserts; break;
    case RowChange::Op::Update: ++stat_.updates; break;
    case RowChange::Op::Delete: ++stat_.deletes; break;
    }

    // Memory bound: the sample is for drill-in/preview only. Beyond the cap we
    // keep counting but stop retaining rows — this is the whole point of
    // replacing the materialized `dml` string.
    if (sample_.size() < kSampleCap) sample_.push_back(std::move(*row));
    else                             truncated_ = true;

    return true;
}

void RowChangeSet::AddFinding(Finding f, ValueVerdict verdict)
{
    if (!MayEmit(verdict)) blocked_ = true;
    findings_.push_back(std::move(f));
}

void RowChangeSet::AddFinding(Finding f)
{
    if (!MayAutoAlter(f.verdict)) blocked_ = true;
    findings_.push_back(std::move(f));
}

// ---------------------------------------------------------------------------
// Rendering — only ever reachable with an already-gated RowChange in hand, and
// only over the capped sample.
// ---------------------------------------------------------------------------

wxString RenderRowDml(const RowChange& row, const RowRenderSpec& spec)
{
    const Dialect d = spec.dialect;

    // Defensive arity check. A mismatch means the caller paired a row with the
    // wrong table's spec; emitting a half-formed statement would be worse than
    // emitting nothing.
    if (row.Key().size() != spec.pkColumns.size()) return wxString();

    wxString where;
    for (size_t i = 0; i < spec.pkColumns.size(); ++i) {
        if (i) where += L" AND ";
        where += QuoteIdent(spec.pkColumns[i], d) + L" = " + RenderLiteral(row.Key()[i], d);
    }

    switch (row.Operation()) {

    case RowChange::Op::Delete:
        if (where.IsEmpty()) return wxString();   // never emit an unfiltered DELETE
        return L"DELETE FROM " + spec.qualifiedTable + L" WHERE " + where;

    case RowChange::Op::Insert: {
        if (row.Values().size() != spec.columns.size()) return wxString();
        wxString cols, vals;
        for (size_t i = 0; i < spec.columns.size(); ++i) {
            if (i) { cols += L", "; vals += L", "; }
            cols += QuoteIdent(spec.columns[i], d);
            vals += RenderLiteral(row.Values()[i], d);
        }
        return L"INSERT INTO " + spec.qualifiedTable + L" (" + cols + L") VALUES (" + vals + L")";
    }

    case RowChange::Op::Update:
    default: {
        if (row.Values().size() != spec.columns.size()) return wxString();
        if (where.IsEmpty()) return wxString();   // never emit an unfiltered UPDATE
        wxString sets;
        for (size_t i = 0; i < spec.columns.size(); ++i) {
            // Primary-key columns are the identity of the row, not payload.
            bool isPk = false;
            for (const auto& pk : spec.pkColumns) if (pk == spec.columns[i]) { isPk = true; break; }
            if (isPk) continue;
            if (!sets.IsEmpty()) sets += L", ";
            sets += QuoteIdent(spec.columns[i], d) + L" = " + RenderLiteral(row.Values()[i], d);
        }
        if (sets.IsEmpty()) return wxString();    // nothing but the key: nothing to update
        return L"UPDATE " + spec.qualifiedTable + L" SET " + sets + L" WHERE " + where;
    }
    }
}

} // namespace db::sync

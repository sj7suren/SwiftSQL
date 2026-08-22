// QueryBuilderModel.cpp — table/join bookkeeping + the SQL generator. See the
// header for the design contract (stable ids, always-valid output).
#include "ui/QueryBuilderModel.h"

#include <set>

namespace ui {

int QueryBuilderModel::AddTable(const wxString& name,
                                const std::vector<QbColumn>& cols,
                                const wxRect& rect)
{
    QbTable t;
    t.id      = nextId_++;
    t.name    = name;
    t.alias   = wxString::Format(L"t%d", nextAlias_++);
    t.rect    = rect;
    t.columns = cols;
    tables.push_back(std::move(t));
    return tables.back().id;
}

void QueryBuilderModel::RemoveTable(int id)
{
    // Drop joins touching this table first (stable-id compare, order-independent).
    for (size_t i = joins.size(); i-- > 0; )
        if (joins[i].leftId == id || joins[i].rightId == id)
            joins.erase(joins.begin() + i);
    for (size_t i = 0; i < tables.size(); ++i)
        if (tables[i].id == id) { tables.erase(tables.begin() + i); return; }
}

void QueryBuilderModel::AddJoin(int leftId, const wxString& leftCol,
                                int rightId, const wxString& rightCol,
                                QbJoinType type)
{
    if (leftId == rightId && leftCol == rightCol) return;   // a column onto itself
    // Reject a duplicate edge (same pair of column endpoints, either orientation).
    for (const auto& j : joins) {
        const bool same = (j.leftId == leftId && j.leftCol == leftCol &&
                           j.rightId == rightId && j.rightCol == rightCol);
        const bool flip = (j.leftId == rightId && j.leftCol == rightCol &&
                           j.rightId == leftId && j.rightCol == leftCol);
        if (same || flip) return;
    }
    QbJoin j;
    j.leftId = leftId; j.leftCol = leftCol;
    j.rightId = rightId; j.rightCol = rightCol;
    j.type = type;
    joins.push_back(std::move(j));
}

void QueryBuilderModel::RemoveJoin(size_t index)
{
    if (index < joins.size()) joins.erase(joins.begin() + index);
}

QbTable* QueryBuilderModel::TableById(int id)
{
    for (auto& t : tables) if (t.id == id) return &t;
    return nullptr;
}

const QbTable* QueryBuilderModel::TableById(int id) const
{
    for (const auto& t : tables) if (t.id == id) return &t;
    return nullptr;
}

// ---------------------------------------------------------------------------
wxString QueryBuilderModel::BuildSql() const
{
    if (tables.empty()) return wxString();

    auto Q = [this](const wxString& s) { return db::QuoteIdent(s, dialect); };

    // FROM/JOIN reference: [`db`.]`table` AS `alias`.
    auto tableRef = [&](const QbTable& t) -> wxString {
        wxString ref = (qualifyDb && !database.IsEmpty())
                     ? Q(database) + L"." + Q(t.name)
                     : Q(t.name);
        return ref + L" " + Q(t.alias);
    };
    // Column reference: `alias`.`col`.
    auto colRef = [&](const QbTable& t, const wxString& col) -> wxString {
        return Q(t.alias) + L"." + Q(col);
    };

    // ---- SELECT list: every checked column, in (table, column) order ----
    std::vector<wxString> selCols;
    for (const auto& t : tables)
        for (const auto& c : t.columns)
            if (c.selected) selCols.push_back(colRef(t, c.name));

    wxString select = L"*";
    if (!selCols.empty()) {
        select.clear();
        for (size_t i = 0; i < selCols.size(); ++i) {
            if (i) select += L",\n       ";
            select += selCols[i];
        }
    }

    wxString sql = L"SELECT " + select + L"\n  FROM " + tableRef(tables[0]);

    // ---- chain the remaining tables on with JOINs ----
    std::set<int> emitted;
    emitted.insert(tables[0].id);
    for (size_t i = 1; i < tables.size(); ++i) {
        const QbTable& t = tables[i];
        std::vector<wxString> conds;
        QbJoinType jt = QbJoinType::Inner;
        for (const auto& j : joins) {
            const bool involvesT = (j.leftId == t.id || j.rightId == t.id);
            if (!involvesT) continue;
            const int other = (j.leftId == t.id) ? j.rightId : j.leftId;
            if (!emitted.count(other)) continue;   // its partner isn't in the tree yet
            const QbTable* lt = TableById(j.leftId);
            const QbTable* rt = TableById(j.rightId);
            if (!lt || !rt) continue;               // defensive; ids are kept consistent
            conds.push_back(colRef(*lt, j.leftCol) + L" = " + colRef(*rt, j.rightCol));
            jt = j.type;                            // last edge's type wins the keyword
        }
        if (!conds.empty()) {
            wxString on;
            for (size_t k = 0; k < conds.size(); ++k) {
                if (k) on += L" AND ";
                on += conds[k];
            }
            sql += L"\n  " + QbJoinKeyword(jt) + L" " + tableRef(t) + L" ON " + on;
        } else {
            // No edge to the emitted set → a cross join keeps the SQL valid.
            sql += L"\n  CROSS JOIN " + tableRef(t);
        }
        emitted.insert(t.id);
    }
    return sql;
}

} // namespace ui

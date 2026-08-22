// MermaidDiagram.cpp — pure engine logic: parse, layer, lay out.
//
// No GUI headers here on purpose: this translation unit depends only on wxString
// + the POD geometry in MermaidDiagram.h, so the head-less unit test compiles it
// directly and links swiftsql::core. All wxGraphicsContext drawing lives in the
// sibling MermaidRender.cpp.
//
// ── Parser (Mermaid flowchart subset) ─────────────────────────────────────
//   direction : first `flowchart|graph  TD|TB|LR|RL|BT` line (default TD)
//   nodes     : id[..] rect · id(..) round · id{..} diamond · id((..)) circle
//               (a bare id first seen inside an edge → default Rect, label=id)
//   edges     : A --> B (arrow) · A --- B (line) · A -.-> B (dashed) ·
//               A ==> B (thick→solid) · A -->|txt| B (label) · chains A-->B-->C
//   ignored   : %% comments, subgraph/end/class/classDef/style/linkStyle/click…
//
// ── Layering ──────────────────────────────────────────────────────────────
//   longest-path: DFS marks back edges (cycle → ignored so no hang); Kahn topo
//   over the remaining DAG relaxes rank[v] = max(rank[u]+1). Sources = rank 0.
//
// ── Layout ────────────────────────────────────────────────────────────────
//   node size from injected measurer + padding (diamonds widened); ranks stack
//   along the flow axis, siblings centred on the cross axis. RL/BT mirror.

#include "ui/MermaidDiagram.h"

#include <wx/arrstr.h>
#include <algorithm>
#include <map>

namespace ui {
namespace mmd {

int MMGraph::IndexOf(const wxString& id) const
{
    for (size_t i = 0; i < nodes.size(); ++i)
        if (nodes[i].id == id) return static_cast<int>(i);
    return -1;
}

namespace {

// --------------------------------------------------------------------------
// Small character helpers (locale-independent; wxString is UTF-16 on MSW).
// --------------------------------------------------------------------------
bool IsIdStart(wxUniChar c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}
bool IsConnChar(wxUniChar c)
{
    return c == '-' || c == '.' || c == '=' || c == '>' || c == '<' ||
           c == 'o' || c == 'x' || c == '~';
}

wxString Trim(const wxString& s)
{
    wxString t = s;
    t.Trim(true).Trim(false);
    return t;
}

// Strip an optional pair of surrounding quotes from a node/edge label.
wxString Unquote(const wxString& s)
{
    wxString t = Trim(s);
    if (t.length() >= 2 && (t[0] == '"' || t[0] == '\'') && t.Last() == t[0])
        return t.Mid(1, t.length() - 2);
    return t;
}

// --------------------------------------------------------------------------
// Parser state — builds a MMGraph incrementally, de-duplicating node ids.
// --------------------------------------------------------------------------
struct Builder {
    MMGraph& g;

    int Intern(const wxString& id, bool hasSpec, const wxString& label, MMShape sh)
    {
        int idx = g.IndexOf(id);
        if (idx < 0) {
            if (static_cast<int>(g.nodes.size()) >= kMaxNodes) return -1;
            MMNode n;
            n.id    = id;
            n.label = hasSpec ? label : id;
            n.shape = sh;
            g.nodes.push_back(std::move(n));
            return static_cast<int>(g.nodes.size()) - 1;
        }
        // A later occurrence carrying an explicit shape/label wins over a bare
        // reference that merely mentioned the id.
        if (hasSpec) {
            g.nodes[idx].label = label;
            g.nodes[idx].shape = sh;
        }
        return idx;
    }

    void AddEdge(int from, int to, const wxString& label, bool dashed, bool arrow)
    {
        if (from < 0 || to < 0 || from == to) return;
        if (static_cast<int>(g.edges.size()) >= kMaxEdges) return;
        MMEdge e;
        e.from = from; e.to = to; e.label = label; e.dashed = dashed; e.arrow = arrow;
        g.edges.push_back(std::move(e));
    }
};

// Read a node spec starting at `i`. Returns node index (interned) or -1 if the
// cursor is not at a node. Advances `i` past the spec.
int ReadNode(Builder& b, const wxString& s, size_t& i)
{
    const size_t n = s.length();
    while (i < n && s[i] == ' ') ++i;
    if (i >= n || !IsIdStart(s[i])) return -1;

    size_t start = i;
    while (i < n && IsIdStart(s[i])) ++i;
    wxString id = s.Mid(start, i - start);

    // Optional shape wrapper immediately after the id (no space).
    MMShape  shape   = MMShape::Rect;
    wxString label;
    bool     hasSpec = false;

    if (i < n) {
        wxUniChar c = s[i];
        wxString open, close;
        if (c == '[') {
            shape = MMShape::Rect;
            if (i + 1 < n && s[i + 1] == '[') { open = L"[["; close = L"]]"; }
            else                              { open = L"[";  close = L"]";  }
        } else if (c == '(') {
            if (i + 1 < n && s[i + 1] == '(') { shape = MMShape::Circle; open = L"(("; close = L"))"; }
            else if (i + 1 < n && s[i + 1] == '[') { shape = MMShape::Round; open = L"(["; close = L"])"; }
            else                              { shape = MMShape::Round;  open = L"(";  close = L")";  }
        } else if (c == '{') {
            shape = MMShape::Diamond;
            if (i + 1 < n && s[i + 1] == '{') { open = L"{{"; close = L"}}"; }
            else                              { open = L"{";  close = L"}";  }
        }
        if (!open.empty()) {
            size_t bodyStart = i + open.length();
            size_t found = s.find(close, bodyStart);
            if (found != wxString::npos) {
                label   = Unquote(s.Mid(bodyStart, found - bodyStart));
                i       = found + close.length();
                hasSpec = true;
            }
        }
    }
    return b.Intern(id, hasSpec, label, shape);
}

// Read a connector at `i`: token run of connector chars, then optional |label|.
// Fills dashed/arrow/label; returns true and advances `i`, or false if none.
bool ReadConnector(const wxString& s, size_t& i, bool& dashed, bool& arrow,
                   wxString& label)
{
    const size_t n = s.length();
    while (i < n && s[i] == ' ') ++i;
    if (i >= n || !IsConnChar(s[i])) return false;
    // A connector must contain '-', '.', '=' or '~' — a lone '>' is not one.
    size_t start = i;
    while (i < n && IsConnChar(s[i])) ++i;
    wxString conn = s.Mid(start, i - start);
    if (conn.find_first_of(L"-.=~") == wxString::npos) { i = start; return false; }

    dashed = conn.find('.') != wxString::npos || conn.find('~') != wxString::npos;
    arrow  = conn.find('>') != wxString::npos || conn.find('<') != wxString::npos ||
             conn.find('o') != wxString::npos || conn.find('x') != wxString::npos;

    label.clear();
    while (i < n && s[i] == ' ') ++i;
    if (i < n && s[i] == '|') {                    // -->|label|
        size_t close = s.find('|', i + 1);
        if (close != wxString::npos) {
            label = Unquote(s.Mid(i + 1, close - (i + 1)));
            i = close + 1;
        }
    }
    return true;
}

bool StartsWithWord(const wxString& s, const wxChar* w)
{
    wxString word(w);
    if (!s.StartsWith(word)) return false;
    size_t k = word.length();
    return k >= s.length() || s[k] == ' ' || s[k] == '\t';
}

bool IsIgnorableLine(const wxString& s)
{
    static const wxChar* kSkip[] = {
        L"subgraph", L"end", L"class", L"classDef", L"style", L"linkStyle",
        L"click", L"direction", L"%%", nullptr,
    };
    for (int k = 0; kSkip[k]; ++k)
        if (StartsWithWord(s, kSkip[k]) || s.StartsWith(kSkip[k])) return true;
    return false;
}

MMDir ParseDir(const wxString& tok)
{
    wxString t = tok.Upper();
    if (t == "LR") return MMDir::LR;
    if (t == "RL") return MMDir::RL;
    if (t == "BT") return MMDir::BT;
    return MMDir::TD;  // TD / TB / anything else
}

void ParseLine(Builder& b, const wxString& raw)
{
    wxString line = Trim(raw);
    if (line.empty() || IsIgnorableLine(line)) return;
    if (line.EndsWith(";")) line = Trim(line.Left(line.length() - 1));

    size_t i = 0;
    int prev = ReadNode(b, line, i);
    if (prev < 0) return;                          // not an edge/node line

    // Chain: node (connector node)*  → one edge per hop.
    bool dashed, arrow; wxString lbl;
    size_t guard = 0;
    while (ReadConnector(line, i, dashed, arrow, lbl)) {
        int next = ReadNode(b, line, i);
        if (next < 0) break;
        b.AddEdge(prev, next, lbl, dashed, arrow);
        prev = next;
        if (++guard > kMaxEdges) break;
    }
}

} // namespace

// ==========================================================================
// ParseMermaid
// ==========================================================================
MMGraph ParseMermaid(const wxString& src)
{
    MMGraph g;
    Builder b{g};

    wxArrayString lines = wxSplit(src, '\n');
    bool sawHeader = false;

    for (size_t li = 0; li < lines.size(); ++li) {
        wxString line = Trim(lines[li]);
        if (line.empty()) continue;

        if (!sawHeader) {
            // Accept the leading `flowchart TD` / `graph LR` header; pull dir.
            wxString low = line.Lower();
            if (low.StartsWith("flowchart") || low.StartsWith("graph")) {
                wxArrayString toks = wxSplit(line, ' ');
                if (toks.size() >= 2) g.dir = ParseDir(Trim(toks[1]));
                sawHeader = true;
                continue;
            }
            sawHeader = true;   // no header → default TD, parse this line as body
        }
        ParseLine(b, line);
    }
    return g;
}

// ==========================================================================
// AssignRanks — longest path, back edges (cycles) removed via DFS colouring.
// ==========================================================================
std::vector<int> AssignRanks(const MMGraph& g)
{
    const int n = static_cast<int>(g.nodes.size());
    std::vector<int> rank(n, 0);
    if (n == 0) return rank;

    std::vector<std::vector<int>> adj(n);
    for (const auto& e : g.edges)
        if (e.from >= 0 && e.to >= 0 && e.from < n && e.to < n && e.from != e.to)
            adj[e.from].push_back(e.to);

    // DFS colouring: 0 white / 1 grey (on stack) / 2 black. Edge into a grey
    // node is a back edge → excluded from the DAG so ranking cannot loop.
    std::vector<char> color(n, 0);
    std::vector<char> isBack(g.edges.size(), 0);
    // Iterative DFS to avoid deep recursion on large chains.
    for (int s = 0; s < n; ++s) {
        if (color[s]) continue;
        std::vector<std::pair<int, size_t>> stk; // (node, next adj index)
        color[s] = 1;
        stk.push_back({s, 0});
        while (!stk.empty()) {
            int u = stk.back().first;
            size_t& k = stk.back().second;
            if (k < adj[u].size()) {
                int v = adj[u][k++];
                if (color[v] == 1) {
                    // mark the matching edge(s) u->v as back edges
                    for (size_t ei = 0; ei < g.edges.size(); ++ei)
                        if (g.edges[ei].from == u && g.edges[ei].to == v) isBack[ei] = 1;
                } else if (color[v] == 0) {
                    color[v] = 1;
                    stk.push_back({v, 0});
                }
            } else {
                color[u] = 2;
                stk.pop_back();
            }
        }
    }

    // Build the acyclic adjacency + indegree from non-back edges.
    std::vector<std::vector<int>> dag(n);
    std::vector<int> indeg(n, 0);
    for (size_t ei = 0; ei < g.edges.size(); ++ei) {
        if (isBack[ei]) continue;
        const auto& e = g.edges[ei];
        if (e.from < 0 || e.to < 0 || e.from >= n || e.to >= n || e.from == e.to)
            continue;
        dag[e.from].push_back(e.to);
        indeg[e.to]++;
    }

    // Kahn topological order; relax longest path.
    std::vector<int> q;
    for (int v = 0; v < n; ++v) if (indeg[v] == 0) q.push_back(v);
    size_t head = 0;
    std::vector<int> deg = indeg;
    while (head < q.size()) {
        int u = q[head++];
        for (int v : dag[u]) {
            rank[v] = std::max(rank[v], rank[u] + 1);
            if (--deg[v] == 0) q.push_back(v);
        }
    }
    return rank;
}

// ==========================================================================
// LayoutGraph — ranks along the flow axis, siblings centred on the cross axis.
// ==========================================================================
namespace {
constexpr double kPadX     = 20;   // horizontal text padding inside a node
constexpr double kPadY     = 12;
constexpr double kMinW     = 46;
constexpr double kMinH     = 34;
constexpr double kRankGap  = 66;   // gap between successive ranks (flow axis)
constexpr double kNodeGap  = 34;   // gap between siblings (cross axis)
constexpr double kMargin   = 26;
constexpr double kDiamondK = 1.5;  // diamonds need extra width for the points
} // namespace

MMLayout LayoutGraph(const MMGraph& g, const MMMeasurer& measure)
{
    MMLayout out;
    const int n = static_cast<int>(g.nodes.size());
    out.nodes.resize(n);
    if (n == 0) { out.width = 120; out.height = 60; return out; }

    // 1) node sizes from the measurer + shape-aware padding.
    std::vector<MMSize> sz(n);
    for (int i = 0; i < n; ++i) {
        MMSize t = measure ? measure(g.nodes[i].label) : MMSize{40, 16};
        double w = t.w + 2 * kPadX;
        double h = t.h + 2 * kPadY;
        if (g.nodes[i].shape == MMShape::Diamond) { w *= kDiamondK; h += 8; }
        if (g.nodes[i].shape == MMShape::Circle)  { w = std::max(w, h) ;
                                                    w = h = std::max(w, h); }
        sz[i].w = std::max(w, kMinW);
        sz[i].h = std::max(h, kMinH);
    }

    // 2) group node indices by rank, preserving first-appearance order.
    std::vector<int> rank = AssignRanks(g);
    int maxRank = 0;
    for (int r : rank) maxRank = std::max(maxRank, r);
    std::vector<std::vector<int>> byRank(maxRank + 1);
    for (int i = 0; i < n; ++i) byRank[rank[i]].push_back(i);

    const bool horizMain = (g.dir == MMDir::LR || g.dir == MMDir::RL);
    auto mainSize  = [&](int i){ return horizMain ? sz[i].w : sz[i].h; };
    auto crossSize = [&](int i){ return horizMain ? sz[i].h : sz[i].w; };

    // 3) per-rank cross length + main extent; overall cross span for centring.
    std::vector<double> rankCross(byRank.size(), 0);
    std::vector<double> rankMain(byRank.size(), 0);
    double crossSpan = 0;
    for (size_t r = 0; r < byRank.size(); ++r) {
        double c = 0, m = 0;
        for (size_t k = 0; k < byRank[r].size(); ++k) {
            int i = byRank[r][k];
            if (k) c += kNodeGap;
            c += crossSize(i);
            m = std::max(m, mainSize(i));
        }
        rankCross[r] = c;
        rankMain[r]  = m;
        crossSpan    = std::max(crossSpan, c);
    }

    // 4) place: main offset accumulates per rank; cross centred within span.
    std::vector<double> mainPos(byRank.size(), 0);
    double mainCursor = kMargin;
    for (size_t r = 0; r < byRank.size(); ++r) {
        mainPos[r] = mainCursor;
        mainCursor += rankMain[r] + kRankGap;
    }

    for (size_t r = 0; r < byRank.size(); ++r) {
        double cross = kMargin + (crossSpan - rankCross[r]) / 2.0;
        for (int i : byRank[r]) {
            double mainCoord  = mainPos[r] + (rankMain[r] - mainSize(i)) / 2.0;
            MMRect rc;
            if (horizMain) { rc.x = mainCoord; rc.y = cross; }
            else           { rc.x = cross;     rc.y = mainCoord; }
            rc.w = sz[i].w; rc.h = sz[i].h;
            out.nodes[i] = rc;
            cross += crossSize(i) + kNodeGap;
        }
    }

    double totalMain = mainCursor - kRankGap + kMargin;
    double totalCross = crossSpan + 2 * kMargin;
    out.width  = horizMain ? totalMain  : totalCross;
    out.height = horizMain ? totalCross : totalMain;

    // 5) mirror for RL (flip x) / BT (flip y).
    if (g.dir == MMDir::RL) {
        for (auto& r : out.nodes) r.x = out.width - r.x - r.w;
    } else if (g.dir == MMDir::BT) {
        for (auto& r : out.nodes) r.y = out.height - r.y - r.h;
    }
    return out;
}

} // namespace mmd
} // namespace ui

// mermaid_test.cpp — unit tests for the Mermaid flowchart engine's pure half:
// ParseMermaid (text → nodes/edges/shapes/direction) and AssignRanks (longest-
// path layering with back-edge removal). Rendering (wxGraphicsContext) is a thin
// GUI layer and is intentionally NOT exercised here — only the deterministic
// parse + layout outputs are asserted.
//
// Harness: dependency-free assert style, mirroring sqlscript_test.cpp. The test
// compiles ui/MermaidDiagram.cpp directly (pure logic, wxString-only) and links
// swiftsql::core for wxString + the src/ include root — no GUI, head-less safe.

#include "ui/MermaidDiagram.h"

#include <cstdio>
#include <wx/string.h>

using namespace ui::mmd;

static int g_checks = 0;
static int g_fails  = 0;

static void ExpectI(const char* name, long got, long want)
{
    ++g_checks;
    if (got != want) {
        ++g_fails;
        std::printf("  FAIL %s  want=%ld got=%ld\n", name, want, got);
    } else {
        std::printf("  ok   %s\n", name);
    }
}

static void ExpectTrue(const char* name, bool cond)
{
    ++g_checks;
    if (!cond) { ++g_fails; std::printf("  FAIL %s\n", name); }
    else       { std::printf("  ok   %s\n", name); }
}

// ---- graph query helpers --------------------------------------------------
static MMShape ShapeOf(const MMGraph& g, const char* id)
{
    int i = g.IndexOf(wxString::FromUTF8(id));
    return i < 0 ? MMShape::Rect : g.nodes[i].shape;
}
static wxString LabelOf(const MMGraph& g, const char* id)
{
    int i = g.IndexOf(wxString::FromUTF8(id));
    return i < 0 ? wxString() : g.nodes[i].label;
}
// find an edge id→id; returns index or -1
static int EdgeIdx(const MMGraph& g, const char* from, const char* to)
{
    int a = g.IndexOf(wxString::FromUTF8(from));
    int b = g.IndexOf(wxString::FromUTF8(to));
    for (size_t i = 0; i < g.edges.size(); ++i)
        if (g.edges[i].from == a && g.edges[i].to == b) return static_cast<int>(i);
    return -1;
}
static int RankOf(const MMGraph& g, const std::vector<int>& r, const char* id)
{
    int i = g.IndexOf(wxString::FromUTF8(id));
    return i < 0 ? -1 : r[i];
}

// ==========================================================================
static void TestShapesAndDirection()
{
    std::printf("[shapes & direction]\n");
    const wchar_t* src =
        L"flowchart LR\n"
        L"  A[Start]\n"
        L"  B(Rounded)\n"
        L"  C{Decision}\n"
        L"  D((End))\n"
        L"  A --> B\n"
        L"  B --> C\n"
        L"  C --> D\n";
    MMGraph g = ParseMermaid(src);

    ExpectTrue("direction LR parsed", g.dir == MMDir::LR);
    ExpectI("4 nodes", (long)g.nodes.size(), 4);
    ExpectI("3 edges", (long)g.edges.size(), 3);
    ExpectTrue("A is rect",    ShapeOf(g, "A") == MMShape::Rect);
    ExpectTrue("B is round",   ShapeOf(g, "B") == MMShape::Round);
    ExpectTrue("C is diamond", ShapeOf(g, "C") == MMShape::Diamond);
    ExpectTrue("D is circle",  ShapeOf(g, "D") == MMShape::Circle);
    ExpectTrue("A label = Start", LabelOf(g, "A") == "Start");
    ExpectTrue("D label = End",   LabelOf(g, "D") == "End");
}

static void TestEdgeKindsAndLabels()
{
    std::printf("[edge kinds & labels]\n");
    const wchar_t* src =
        L"flowchart TD\n"
        L"A -->|yes| B\n"
        L"A --- C\n"
        L"C -.-> D\n";
    MMGraph g = ParseMermaid(src);

    ExpectI("default header omitted still 4 nodes", (long)g.nodes.size(), 4);
    ExpectI("3 edges", (long)g.edges.size(), 3);

    int e1 = EdgeIdx(g, "A", "B");
    ExpectTrue("A->B exists", e1 >= 0);
    ExpectTrue("A->B has label 'yes'", e1 >= 0 && g.edges[e1].label == "yes");
    ExpectTrue("A->B has arrow",       e1 >= 0 && g.edges[e1].arrow);
    ExpectTrue("A->B solid",           e1 >= 0 && !g.edges[e1].dashed);

    int e2 = EdgeIdx(g, "A", "C");
    ExpectTrue("A---C exists no arrow", e2 >= 0 && !g.edges[e2].arrow);

    int e3 = EdgeIdx(g, "C", "D");
    ExpectTrue("C-.->D dashed arrow", e3 >= 0 && g.edges[e3].dashed && g.edges[e3].arrow);
}

static void TestChainAndImplicitNodes()
{
    std::printf("[chain & implicit nodes]\n");
    // Chain splits into hops; nodes only ever named in edges default to rect+id.
    MMGraph g = ParseMermaid(L"flowchart TD\nA --> B --> C --> D\n");
    ExpectI("chain: 4 nodes", (long)g.nodes.size(), 4);
    ExpectI("chain: 3 edges", (long)g.edges.size(), 3);
    ExpectTrue("A->B", EdgeIdx(g, "A", "B") >= 0);
    ExpectTrue("B->C", EdgeIdx(g, "B", "C") >= 0);
    ExpectTrue("C->D", EdgeIdx(g, "C", "D") >= 0);
    ExpectTrue("implicit label = id", LabelOf(g, "A") == "A");
}

static void TestUnknownLinesSkipped()
{
    std::printf("[unknown lines skipped]\n");
    const wchar_t* src =
        L"flowchart TD\n"
        L"%% this is a comment\n"
        L"classDef big fill:#f00;\n"
        L"subgraph one\n"
        L"  A --> B\n"
        L"end\n"
        L"style A fill:#0f0\n"
        L"A --> C\n";
    MMGraph g = ParseMermaid(src);
    ExpectI("3 nodes (A,B,C)", (long)g.nodes.size(), 3);
    ExpectI("2 edges (A-B, A-C)", (long)g.edges.size(), 2);
}

static void TestRanksLinear()
{
    std::printf("[ranks: linear]\n");
    MMGraph g = ParseMermaid(L"flowchart TD\nA --> B --> C --> D\n");
    auto r = AssignRanks(g);
    ExpectI("rank A", RankOf(g, r, "A"), 0);
    ExpectI("rank B", RankOf(g, r, "B"), 1);
    ExpectI("rank C", RankOf(g, r, "C"), 2);
    ExpectI("rank D", RankOf(g, r, "D"), 3);
}

static void TestRanksLongestPath()
{
    std::printf("[ranks: longest path (diamond)]\n");
    // A->B->D and A->C->D and A->D : D must sit at the LONGEST path (rank 2),
    // not the shortest (rank 1 via the direct A->D edge).
    const wchar_t* src =
        L"flowchart TD\n"
        L"A --> B\n"
        L"A --> C\n"
        L"B --> D\n"
        L"C --> D\n"
        L"A --> D\n";
    MMGraph g = ParseMermaid(src);
    auto r = AssignRanks(g);
    ExpectI("rank A", RankOf(g, r, "A"), 0);
    ExpectI("rank B", RankOf(g, r, "B"), 1);
    ExpectI("rank C", RankOf(g, r, "C"), 1);
    ExpectI("rank D = longest (2)", RankOf(g, r, "D"), 2);
}

static void TestRanksCycleNoHang()
{
    std::printf("[ranks: cycle back-edge removed, no hang]\n");
    // A->B->C->A is a cycle; the back edge C->A is dropped so ranking terminates.
    MMGraph g = ParseMermaid(L"flowchart TD\nA --> B\nB --> C\nC --> A\n");
    auto r = AssignRanks(g);
    ExpectI("3 nodes", (long)g.nodes.size(), 3);
    ExpectI("rank A", RankOf(g, r, "A"), 0);
    ExpectI("rank B", RankOf(g, r, "B"), 1);
    ExpectI("rank C", RankOf(g, r, "C"), 2);
}

static void TestEmptyAndGarbage()
{
    std::printf("[empty & garbage]\n");
    MMGraph e = ParseMermaid(L"");
    ExpectI("empty src -> 0 nodes", (long)e.nodes.size(), 0);
    ExpectTrue("empty graph.empty()", e.empty());

    MMGraph g = ParseMermaid(L"this is not mermaid at all\n!!!\n");
    // "this" parses as an id token but no connector follows → no edges; a lone
    // node line without brackets/edges is not interned, so no spurious nodes.
    ExpectI("garbage -> 0 edges", (long)g.edges.size(), 0);
}

int main()
{
    std::printf("== mermaid_test ==\n");
    TestShapesAndDirection();
    TestEdgeKindsAndLabels();
    TestChainAndImplicitNodes();
    TestUnknownLinesSkipped();
    TestRanksLinear();
    TestRanksLongestPath();
    TestRanksCycleNoHang();
    TestEmptyAndGarbage();
    std::printf("== %d checks, %d failed ==\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}

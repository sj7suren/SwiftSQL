// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// MermaidDiagram.h — a self-contained, dependency-free engine that renders a
// subset of Mermaid `flowchart` text to a wxBitmap.
//
// Phase 1 scope (this file): the ENGINE only — parse → layer → lay out → paint.
// It is deliberately decoupled from the chat / AI code: `MermaidDiagram` eats a
// wxString and yields data structures + a bitmap, nothing else.
//
// Layering of concerns (so the logic is unit-testable head-less):
//   * ParseMermaid()  — pure text → MMGraph            (MermaidDiagram.cpp)
//   * AssignRanks()   — pure longest-path layering      (MermaidDiagram.cpp)
//   * LayoutGraph()   — pure coordinate assignment       (MermaidDiagram.cpp)
//                       (text sizing is injected via MMMeasurer, so no GUI dep)
//   * RenderToBitmap()— wxGraphicsContext drawing         (MermaidRender.cpp)
//
// The first three functions depend only on wxString + POD geometry, so a
// head-less test can compile MermaidDiagram.cpp alone and link swiftsql::core.
#pragma once

#include <wx/string.h>
#include <wx/bitmap.h>
#include <functional>
#include <vector>

namespace ui {
namespace mmd {

// ---- shapes recognised by the flowchart node syntax -----------------------
enum class MMShape {
    Rect,     // id[label]      — process / default
    Round,    // id(label)      — rounded / stadium
    Diamond,  // id{label}      — decision
    Circle,   // id((label))    — start / end
};

// ---- flow direction (first line: `flowchart TD|TB|LR|RL|BT`) --------------
enum class MMDir { TD, LR, RL, BT }; // TB is an alias of TD

struct MMNode {
    wxString id;
    wxString label;
    MMShape  shape = MMShape::Rect;
};

struct MMEdge {
    int      from   = -1;   // index into MMGraph::nodes
    int      to     = -1;
    wxString label;
    bool     dashed = false;
    bool     arrow  = true; // `-->` has a head, `---` does not
};

struct MMGraph {
    MMDir               dir = MMDir::TD;
    std::vector<MMNode> nodes;
    std::vector<MMEdge> edges;

    // -1 if not found.
    int IndexOf(const wxString& id) const;
    bool empty() const { return nodes.empty(); }
};

// ---- POD geometry (double precision; no wx/gdicmn dependency here) ---------
struct MMSize { double w = 0, h = 0; };
struct MMRect { double x = 0, y = 0, w = 0, h = 0; };

// Injected text-measurement callback: text → pixel size at the label font.
// The default (GUI) implementation lives in MermaidRender.cpp; tests can pass a
// deterministic estimator so layout stays reproducible without a display.
using MMMeasurer = std::function<MMSize(const wxString&)>;

struct MMLayout {
    std::vector<MMRect> nodes;   // parallel to MMGraph::nodes
    double width  = 0;           // bounding box (includes margins)
    double height = 0;
};

// ---------------------------------------------------------------------------
// Pure logic (MermaidDiagram.cpp) — no GUI, unit-tested.
// ---------------------------------------------------------------------------

// Parse a flowchart subset. Unrecognised lines/directives are skipped, never
// fatal. Node/edge counts are capped (kMaxNodes / kMaxEdges) to stay bounded.
MMGraph ParseMermaid(const wxString& src);

// Longest-path layering with DFS back-edge removal (so cycles cannot hang the
// ranker). Returns a rank per node, parallel to graph.nodes; isolated nodes = 0.
std::vector<int> AssignRanks(const MMGraph& graph);

// Assign coordinates. `measure` sizes each label; direction picks the axis.
MMLayout LayoutGraph(const MMGraph& graph, const MMMeasurer& measure);

// Bounds (belt-and-braces caps mirrored in ParseMermaid).
constexpr int kMaxNodes = 200;
constexpr int kMaxEdges = 400;

// ---------------------------------------------------------------------------
// GUI rendering (MermaidRender.cpp).
// ---------------------------------------------------------------------------

// Render `src` to a bitmap. If the drawing is wider than `maxWidth` (>0) it is
// scaled down to fit (fit-width). Empty / all-unparseable input yields a small
// placeholder bitmap (still IsOk()==true) — this function never throws/crashes.
wxBitmap RenderToBitmap(const wxString& src, int maxWidth = 0);

// Same, but on an already-parsed graph (lets the future chat layer cache the
// parse). Exposed for the eventual message-rendering integration.
wxBitmap RenderGraphToBitmap(const MMGraph& graph, int maxWidth = 0);

} // namespace mmd
} // namespace ui

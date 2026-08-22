// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

// SplashScreen.h — animated launch / splash window shown before MainFrame.
//
// A borderless, always-on-top, rounded 720x460 card drawn entirely with
// wxGraphicsContext (no image assets). Faithful to docs/UI/SwiftSql.dc.html
// section 02 "Launch — Splash Screen". All animation phases are derived from a
// monotonic tick counter (30fps timer) — no wall-clock / random. After a minimum
// display time (or a click / key press) the finished callback fires once and the
// window destroys itself.
#pragma once

#include <wx/frame.h>
#include <wx/timer.h>

#include <functional>

class wxGraphicsContext;   // fwd (global scope) — full type only needed in .cpp

namespace ui {

class SplashScreen : public wxFrame {
public:
    SplashScreen();

    // Fires exactly once, right before the splash tears itself down. Wire this to
    // reveal the main window. Called on the GUI thread.
    void SetOnFinished(std::function<void()> cb) { onFinished_ = std::move(cb); }

private:
    void OnPaint(wxPaintEvent&);
    void OnEraseBackground(wxEraseEvent&) {}   // swallow to kill flicker
    void OnTimer(wxTimerEvent&);
    void OnMouseDown(wxMouseEvent&);
    void OnCharHook(wxKeyEvent&);

    void ApplyRoundedShape();   // SetShape via a rounded-rect bitmap mask
    void Draw(wxGraphicsContext* gc);
    void Finish();              // idempotent: fire callback, stop, self-destroy

    wxTimer                 timer_;
    long                    tickCount_ = 0;
    bool                    finished_  = false;
    std::function<void()>   onFinished_;
};

}  // namespace ui

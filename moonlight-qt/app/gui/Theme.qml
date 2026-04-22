// VipleStream design tokens.
//
// Mirrors the palette + typography stack defined in the Claude-Design
// mock at temp/design/vs-frame.jsx.  Used as a non-singleton QtObject:
// main.qml instantiates one as `window.theme` and any descendant QML
// can read tokens via `window.theme.lime` etc.
//
// Why not a Singleton? Qt singletons need a qmldir module declaration
// and an import URI — our .pro builds the QML files as a flat qrc
// resource, so adding a singleton would mean refactoring the whole
// QML module layout.  A plain QtObject is the low-disruption path.

import QtQuick 2.9

QtObject {
    // ── Palette ──────────────────────────────────────────────────────
    // Warm-tinted dark ink, off-white paper, electric lime accent.
    readonly property color ink:     "#0D0F0B"   // primary background
    readonly property color ink2:    "#14170F"   // raised surface
    readonly property color ink3:    "#1C1F16"   // elevated surface / track
    readonly property color paper:   "#F2F5E1"   // primary text on dark
    readonly property color paper2:  "#DEE2CC"   // secondary text
    readonly property color lime:    "#D4FF3A"   // accent (CTA, live dot, focus)
    readonly property color limeDim: "#A6CF1E"   // pressed / dim variant
    readonly property color limeInk: "#1A2300"   // text on lime
    readonly property color danger:  "#FF5A4E"   // error, unpaired
    readonly property color mute:    "#8B8E7E"   // ~55% paper, opaque (Qt can't do alpha in color comparisons easily)
    readonly property color mute2:   "#5A5C52"   // ~35% paper
    readonly property color line:    "#1F2219"   // hairline on ink2 (≈10% paper)
    readonly property color line2:   "#2D3127"   // visible hairline (≈18% paper)

    // ── Typography ───────────────────────────────────────────────────
    // We don't ship the Google Fonts (Space Grotesk / IBM Plex Mono /
    // Inter) with the app — they'd need .ttf bundling + a QFontDatabase
    // register at startup.  Fall back to the closest system stacks so
    // the look reads as "editorial magazine" even without the exact
    // fonts installed.
    readonly property string fontDisplay: "Space Grotesk, Segoe UI, Helvetica Neue, Arial, sans-serif"
    readonly property string fontMono:    "IBM Plex Mono, Consolas, Menlo, monospace"
    readonly property string fontBody:    "Inter, Segoe UI, Helvetica Neue, Arial, sans-serif"

    // ── Spacing rhythm ───────────────────────────────────────────────
    readonly property int ruleGap:    10
    readonly property int sectionGap: 28
}

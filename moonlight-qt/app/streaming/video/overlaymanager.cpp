#include "overlaymanager.h"
#include "path.h"

#include <QFile>
#include <QStringList>

using namespace Overlay;

namespace {
// VipleStream editorial HUD palette (ARGB8888 for SDL_FillRect +
// SDL_CreateRGBSurfaceWithFormat). Keep in sync with vs-frame.jsx
// and moonlight-qt/app/gui/Theme.qml.
constexpr Uint32 VS_INK2_A   = 0xDC141710u;  // ~220/255 alpha on ink2
constexpr Uint32 VS_LINE2_A  = 0xFF2D3127u;
constexpr SDL_Color VS_PAPER = {0xF2, 0xF5, 0xE1, 0xFF};
constexpr SDL_Color VS_MUTE  = {0x8B, 0x8E, 0x7E, 0xFF};
constexpr SDL_Color VS_LIME  = {0xD4, 0xFF, 0x3A, 0xFF};

// Compose a §05-style HUD panel from a raw perf-stats text blob.
// Each line in `text` is split on the first ':' into (label, value).
// The resulting SDL_Surface has:
//   • Translucent ink2 rectangle with lime hairline frame
//   • "§ 05 · NET · HUD" mono lime strapline at the top
//   • One row per line: mute-mono label + lime-bold value, separated
//     by a hairline.
// Returns a freshly-allocated SDL_Surface (caller takes ownership)
// or nullptr on allocation failure. Falls back to a single Blended_
// Wrapped render from notifyOverlayUpdated() if this returns null.
SDL_Surface* buildPerfHudSurface(TTF_Font* font, const char* text)
{
    if (!font || !text || !*text) return nullptr;

    // Split into lines + key/value pairs up front so we can size the
    // panel precisely.
    QStringList lines = QString::fromUtf8(text).split('\n', Qt::SkipEmptyParts);
    if (lines.isEmpty()) return nullptr;

    struct Row { QByteArray label; QByteArray value; };
    QList<Row> rows;
    int labelW = 0, valueW = 0, lineH = 0;
    for (const QString& raw : lines) {
        int colon = raw.indexOf(':');
        Row r;
        if (colon > 0) {
            r.label = raw.left(colon).trimmed().toUtf8();
            r.value = raw.mid(colon + 1).trimmed().toUtf8();
        } else {
            r.label = raw.trimmed().toUtf8();
            r.value = QByteArray();
        }
        // Measure text extents for layout.
        int w = 0, h = 0;
        if (!r.label.isEmpty()) {
            TTF_SizeUTF8(font, r.label.constData(), &w, &h);
            if (w > labelW) labelW = w;
            if (h > lineH)  lineH  = h;
        }
        if (!r.value.isEmpty()) {
            TTF_SizeUTF8(font, r.value.constData(), &w, &h);
            if (w > valueW) valueW = w;
            if (h > lineH)  lineH  = h;
        }
        rows.append(r);
    }
    if (lineH < 16) lineH = 16;

    // Layout constants.
    const int padH     = 18;
    const int padV     = 14;
    const int colGap   = 18;
    const int rowGap   = 6;
    const int strapH   = lineH + 10;
    int W = padH * 2 + labelW + colGap + valueW;
    if (W < 220) W = 220;
    int H = padV + strapH + padV
          + rows.size() * (lineH + rowGap) - rowGap
          + padV;

    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, W, H, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!s) return nullptr;
    SDL_SetSurfaceBlendMode(s, SDL_BLENDMODE_BLEND);

    // Translucent ink2 panel with a 1px lime-line frame.
    SDL_Rect panel = {0, 0, W, H};
    SDL_FillRect(s, &panel, VS_INK2_A);
    SDL_Rect top    = {0,   0,   W, 1};
    SDL_Rect bot    = {0,   H-1, W, 1};
    SDL_Rect left   = {0,   0,   1, H};
    SDL_Rect right  = {W-1, 0,   1, H};
    SDL_FillRect(s, &top,   VS_LINE2_A);
    SDL_FillRect(s, &bot,   VS_LINE2_A);
    SDL_FillRect(s, &left,  VS_LINE2_A);
    SDL_FillRect(s, &right, VS_LINE2_A);

    // Strapline "§ 05 · NET · HUD" in lime.
    {
        SDL_Surface* t = TTF_RenderUTF8_Blended(font, "\xc2\xa7 05 \xc2\xb7 NET \xc2\xb7 HUD", VS_LIME);
        if (t) {
            SDL_Rect dst = {padH, padV, t->w, t->h};
            SDL_BlitSurface(t, nullptr, s, &dst);
            SDL_FreeSurface(t);
        }
        // Hairline under strapline.
        SDL_Rect rule = {padH, padV + strapH - 3, W - padH * 2, 1};
        SDL_FillRect(s, &rule, VS_LINE2_A);
    }

    // Rows.
    int y = padV + strapH + padV - rowGap;
    for (int i = 0; i < rows.size(); ++i) {
        const Row& r = rows[i];
        // Label in mute.
        if (!r.label.isEmpty()) {
            SDL_Surface* t = TTF_RenderUTF8_Blended(font, r.label.constData(), VS_MUTE);
            if (t) {
                SDL_Rect dst = {padH, y, t->w, t->h};
                SDL_BlitSurface(t, nullptr, s, &dst);
                SDL_FreeSurface(t);
            }
        }
        // Value in lime, right-justified within the allotted column.
        if (!r.value.isEmpty()) {
            SDL_Surface* t = TTF_RenderUTF8_Blended(font, r.value.constData(), VS_LIME);
            if (t) {
                SDL_Rect dst = {W - padH - t->w, y, t->w, t->h};
                SDL_BlitSurface(t, nullptr, s, &dst);
                SDL_FreeSurface(t);
            }
        }
        // Hairline between rows (not after last).
        if (i + 1 < rows.size()) {
            SDL_Rect rule = {padH, y + lineH + rowGap / 2, W - padH * 2, 1};
            SDL_FillRect(s, &rule, VS_LINE2_A);
        }
        y += lineH + rowGap;
    }
    (void)VS_PAPER;  // silence unused-warning; reserved for a future variant
    return s;
}
} // namespace

// VipleStream: Try loading a CJK-capable system font for Traditional Chinese overlay.
// Falls back to the built-in ModeSeven.ttf if the system font isn't available.
static QByteArray loadCJKFontData()
{
#ifdef _WIN32
    // Microsoft JhengHei (微軟正黑體) — available on all modern Windows
    QFile cjkFont("C:/Windows/Fonts/msjh.ttc");
    if (cjkFont.open(QIODevice::ReadOnly)) {
        QByteArray data = cjkFont.readAll();
        if (!data.isEmpty()) {
            return data;
        }
    }
#endif
    // Fallback to built-in ASCII font
    return Path::readDataFile("ModeSeven.ttf");
}

OverlayManager::OverlayManager() :
    m_Renderer(nullptr),
    m_FontData(loadCJKFontData())
{
    memset(m_Overlays, 0, sizeof(m_Overlays));

    // VipleStream in-stream HUD palette (editorial dark / electric-lime).
    //
    // The SDL/TTF overlay blits raw text surfaces onto the video plane,
    // so the only per-overlay look-and-feel knobs we have are colour +
    // font size. For the full §05 Safe HUD mock (ping-big-lime readout,
    // perf bar strip, bottom quick-menu chips) we'd need a new overlay
    // renderer that composes shapes + text rather than a single text
    // blit — that's a separate, larger change. The colour swaps below
    // at least get the HUD reading in the VS palette.
    //
    // OverlayDebug  — perf stats (was dim yellow #D0D000).
    // Now VS lime (#D4FF3A) to match the in-mock lime data chips.
    m_Overlays[OverlayType::OverlayDebug].color = {0xD4, 0xFF, 0x3A, 0xFF};
    m_Overlays[OverlayType::OverlayDebug].fontSize = 20;

    // OverlayStatusUpdate — transient notices, e.g. "Gamepad mouse mode
    // active". Was dark red (#CC0000), which read as an error. Paper
    // (#F2F5E1) is a neutral status colour and matches the VS foreground.
    m_Overlays[OverlayType::OverlayStatusUpdate].color = {0xF2, 0xF5, 0xE1, 0xFF};
    m_Overlays[OverlayType::OverlayStatusUpdate].fontSize = 36;

    // While TTF will usually not be initialized here, it is valid for that not to
    // be the case, since Session destruction is deferred and could overlap with
    // the lifetime of a new Session object.
    //SDL_assert(TTF_WasInit() == 0);

    if (TTF_Init() != 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "TTF_Init() failed: %s",
                    TTF_GetError());
        return;
    }
}

OverlayManager::~OverlayManager()
{
    for (int i = 0; i < OverlayType::OverlayMax; i++) {
        if (m_Overlays[i].surface != nullptr) {
            SDL_FreeSurface(m_Overlays[i].surface);
        }
        if (m_Overlays[i].font != nullptr) {
            TTF_CloseFont(m_Overlays[i].font);
        }
    }

    TTF_Quit();

    // For similar reasons to the comment in the constructor, this will usually,
    // but not always, deinitialize TTF. In the cases where Session objects overlap
    // in lifetime, there may be an additional reference on TTF for the new Session
    // that means it will not be cleaned up here.
    //SDL_assert(TTF_WasInit() == 0);
}

bool OverlayManager::isOverlayEnabled(OverlayType type)
{
    return m_Overlays[type].enabled;
}

char* OverlayManager::getOverlayText(OverlayType type)
{
    return m_Overlays[type].text;
}

void OverlayManager::updateOverlayText(OverlayType type, const char* text)
{
    strncpy(m_Overlays[type].text, text, sizeof(m_Overlays[0].text));
    m_Overlays[type].text[getOverlayMaxTextLength() - 1] = '\0';

    setOverlayTextUpdated(type);
}

int OverlayManager::getOverlayMaxTextLength()
{
    return sizeof(m_Overlays[0].text);
}

int OverlayManager::getOverlayFontSize(OverlayType type)
{
    return m_Overlays[type].fontSize;
}

SDL_Surface* OverlayManager::getUpdatedOverlaySurface(OverlayType type)
{
    // If a new surface is available, return it. If not, return nullptr.
    // Caller must free the surface on success.
    return (SDL_Surface*)SDL_AtomicSetPtr((void**)&m_Overlays[type].surface, nullptr);
}

void OverlayManager::setOverlayTextUpdated(OverlayType type)
{
    // Only update the overlay state if it's enabled. If it's not enabled,
    // the renderer has already been notified by setOverlayState().
    if (m_Overlays[type].enabled) {
        notifyOverlayUpdated(type);
    }
}

void OverlayManager::setOverlayState(OverlayType type, bool enabled)
{
    bool stateChanged = m_Overlays[type].enabled != enabled;

    m_Overlays[type].enabled = enabled;

    if (stateChanged) {
        if (!enabled) {
            // Set the text to empty string on disable
            m_Overlays[type].text[0] = 0;
        }

        notifyOverlayUpdated(type);
    }
}

SDL_Color OverlayManager::getOverlayColor(OverlayType type)
{
    return m_Overlays[type].color;
}

void OverlayManager::setOverlayRenderer(IOverlayRenderer* renderer)
{
    m_Renderer = renderer;
}

void OverlayManager::notifyOverlayUpdated(OverlayType type)
{
    if (m_Renderer == nullptr) {
        return;
    }

    // Construct the required font to render the overlay
    if (m_Overlays[type].font == nullptr) {
        if (m_FontData.isEmpty()) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "SDL overlay font failed to load");
            return;
        }

        // m_FontData must stay around until the font is closed
        m_Overlays[type].font = TTF_OpenFontRW(SDL_RWFromConstMem(m_FontData.constData(), m_FontData.size()),
                                               1,
                                               m_Overlays[type].fontSize);
        if (m_Overlays[type].font == nullptr) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "TTF_OpenFont() failed: %s",
                        TTF_GetError());

            // Can't proceed without a font
            return;
        }
    }

    // Build the new surface. Perf-stats overlay gets the editorial
    // §05 HUD composite (translucent ink2 panel with label/value
    // rows); status-update + anything else falls back to the single
    // Blended_Wrapped text blit.
    SDL_Surface* newSurface = nullptr;
    if (m_Overlays[type].enabled) {
        if (type == OverlayType::OverlayDebug) {
            newSurface = buildPerfHudSurface(m_Overlays[type].font,
                                             m_Overlays[type].text);
        }
        if (newSurface == nullptr) {
            // Fallback: plain text blit. VipleStream CJK support keeps
            // the UTF-8 variant over the ASCII TTF_RenderText_Blended.
            newSurface = TTF_RenderUTF8_Blended_Wrapped(m_Overlays[type].font,
                                                        m_Overlays[type].text,
                                                        m_Overlays[type].color,
                                                        1024);
        }
    }
    SDL_Surface* oldSurface = (SDL_Surface*)SDL_AtomicSetPtr(
        (void**)&m_Overlays[type].surface, newSurface);

    // Notify the renderer
    m_Renderer->notifyOverlayUpdated(type);

    // Free the old surface
    if (oldSurface != nullptr) {
        SDL_FreeSurface(oldSurface);
    }
}

#include "overlaymanager.h"
#include "path.h"

#include <QFile>
#include <QFileInfo>
#include <QStringList>

#include <utility>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

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

// ---------------------------------------------------------------------------
// MappedFile — read-only cross-platform memory mapping.
//
// SDL_ttf takes a `SDL_RWops` over raw memory and dereferences that pointer
// throughout the font's lifetime, so we hand it the mapped region directly
// instead of `readAll()`-ing the font into the heap.  Saves ~15-20 MB of
// resident bytes per OverlayManager.
// ---------------------------------------------------------------------------

MappedFile::MappedFile() noexcept
    :
#ifdef _WIN32
      m_file(nullptr),
      m_mapping(nullptr),
#else
      m_fd(-1),
#endif
      m_data(nullptr),
      m_size(0)
{
}

MappedFile::~MappedFile()
{
    close();
}

MappedFile::MappedFile(MappedFile&& other) noexcept
    : MappedFile()
{
    *this = std::move(other);
}

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept
{
    if (this != &other) {
        close();
#ifdef _WIN32
        m_file = other.m_file;
        m_mapping = other.m_mapping;
        other.m_file = nullptr;
        other.m_mapping = nullptr;
#else
        m_fd = other.m_fd;
        other.m_fd = -1;
#endif
        m_data = other.m_data;
        m_size = other.m_size;
        other.m_data = nullptr;
        other.m_size = 0;
    }
    return *this;
}

bool MappedFile::open(const QString& path)
{
    close();
#ifdef _WIN32
    HANDLE file = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()),
                              GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    LARGE_INTEGER sz;
    if (!GetFileSizeEx(file, &sz) || sz.QuadPart <= 0) {
        CloseHandle(file);
        return false;
    }

    HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY,
                                        0, 0, nullptr);
    if (!mapping) {
        CloseHandle(file);
        return false;
    }

    void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (!view) {
        CloseHandle(mapping);
        CloseHandle(file);
        return false;
    }

    m_file = file;
    m_mapping = mapping;
    m_data = static_cast<const char*>(view);
    m_size = static_cast<qint64>(sz.QuadPart);
    return true;
#else
    const QByteArray pathBytes = QFile::encodeName(path);
    int fd = ::open(pathBytes.constData(), O_RDONLY
#  ifdef O_CLOEXEC
                                          | O_CLOEXEC
#  endif
                    );
    if (fd < 0) {
        return false;
    }

    struct stat st;
    if (::fstat(fd, &st) < 0 || st.st_size <= 0) {
        ::close(fd);
        return false;
    }

    void* view = ::mmap(nullptr, static_cast<size_t>(st.st_size),
                        PROT_READ, MAP_PRIVATE, fd, 0);
    if (view == MAP_FAILED) {
        ::close(fd);
        return false;
    }

    m_fd = fd;
    m_data = static_cast<const char*>(view);
    m_size = static_cast<qint64>(st.st_size);
    return true;
#endif
}

void MappedFile::close()
{
#ifdef _WIN32
    if (m_data) {
        UnmapViewOfFile(m_data);
        m_data = nullptr;
    }
    if (m_mapping) {
        CloseHandle(static_cast<HANDLE>(m_mapping));
        m_mapping = nullptr;
    }
    if (m_file) {
        CloseHandle(static_cast<HANDLE>(m_file));
        m_file = nullptr;
    }
#else
    if (m_data && m_size > 0) {
        ::munmap(const_cast<char*>(m_data), static_cast<size_t>(m_size));
        m_data = nullptr;
    }
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
#endif
    m_size = 0;
}

// VipleStream: Try mapping a CJK-capable system font for Traditional Chinese
// overlay.  Falls back to the built-in ModeSeven.ttf if the system font isn't
// available (caller handles the fallback).
// v1.4.142 — 之前 Linux / macOS 直接 fallback ModeSeven.ttf (ASCII-only retro
// 字體) 導致 overlay 中文都變方框. 新增 Linux Noto / WQY 跟 macOS PingFang
// 候選路徑, 找到就用; 全找不到才退 ModeSeven.
static bool tryMapCJKFont(MappedFile& out)
{
    QStringList candidates;
#ifdef _WIN32
    // Microsoft JhengHei (微軟正黑體, Trad) — available on all modern Windows.
    // 加 YaHei (Simp) / SimSun 當二三順位 fallback, 老版 Windows / Server SKU
    // 缺 JhengHei 時還有得救.
    candidates << "C:/Windows/Fonts/msjh.ttc"      // Microsoft JhengHei (Trad)
               << "C:/Windows/Fonts/msyh.ttc"      // Microsoft YaHei (Simp)
               << "C:/Windows/Fonts/simsun.ttc";   // SimSun
#elif defined(Q_OS_DARWIN)
    candidates << "/System/Library/Fonts/PingFang.ttc"
               << "/System/Library/Fonts/STHeiti Light.ttc"
               << "/Library/Fonts/Songti.ttc";
#else
    // Linux: Noto CJK 在 Ubuntu / Debian / Fedora 都是 default; WQY 是 fallback
    // 給沒裝 Noto 的舊發行版.  Ubuntu deb path 通常是 opentype/noto/, fedora
    // 也是 opentype/, 個別發行版可能落在 truetype/ — 兩個都試.
    candidates << "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"
               << "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc"
               << "/usr/share/fonts/opentype/noto/NotoSerifCJK-Regular.ttc"
               << "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc"
               << "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc"
               << "/usr/share/fonts/truetype/arphic/uming.ttc";
#endif
    for (const QString& path : candidates) {
        if (out.open(path)) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-OVERLAY] CJK system font mmap'd: %s (%lld bytes)",
                        path.toUtf8().constData(),
                        (long long)out.size());
            return true;
        }
    }
    return false;
}

OverlayManager::OverlayManager() :
    m_Renderer(nullptr)
{
    memset(m_Overlays, 0, sizeof(m_Overlays));

    if (!tryMapCJKFont(m_MappedFont)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-OVERLAY] No CJK system font found — falling back to "
                    "built-in ModeSeven.ttf; overlay CJK characters will render as "
                    "boxes. Install: Linux → noto-fonts-cjk; macOS → System default.");
        m_FallbackFont = Path::readDataFile("ModeSeven.ttf");
    }

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
        // Prefer the mmap'd system CJK font; fall back to the embedded
        // ModeSeven.ttf QByteArray when no system font was found.
        const char* fontPtr = nullptr;
        qint64 fontSize = 0;
        if (m_MappedFont.isValid()) {
            fontPtr = m_MappedFont.data();
            fontSize = m_MappedFont.size();
        } else if (!m_FallbackFont.isEmpty()) {
            fontPtr = m_FallbackFont.constData();
            fontSize = m_FallbackFont.size();
        }
        if (fontPtr == nullptr || fontSize <= 0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "SDL overlay font failed to load");
            return;
        }

        // Backing storage (mmap region or QByteArray) outlives OverlayManager,
        // so SDL_ttf can safely hold a pointer into it for the font's lifetime.
        m_Overlays[type].font = TTF_OpenFontRW(SDL_RWFromConstMem(fontPtr, (int)fontSize),
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

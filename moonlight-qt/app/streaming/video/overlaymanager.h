#pragma once

#include <QByteArray>
#include <QString>

#include "SDL_compat.h"
#include <SDL_ttf.h>

namespace Overlay {

enum OverlayType {
    OverlayDebug,
    OverlayStatusUpdate,
    OverlayMax
};

class IOverlayRenderer
{
public:
    virtual ~IOverlayRenderer() = default;

    virtual void notifyOverlayUpdated(OverlayType type) = 0;
};

// Cross-platform read-only memory-mapped file. Used by OverlayManager to
// keep the system CJK font (~15-20 MB) out of heap — the mapping lives
// for the manager's lifetime so SDL_ttf can hold a pointer into it.
// Platform HANDLE / fd are stored as void*/int so this header doesn't
// have to drag in <windows.h> or <fcntl.h>.
class MappedFile
{
public:
    MappedFile() noexcept;
    ~MappedFile();
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
    MappedFile(MappedFile&& other) noexcept;
    MappedFile& operator=(MappedFile&& other) noexcept;

    bool open(const QString& path);
    void close();
    bool isValid() const { return m_data != nullptr && m_size > 0; }
    const char* data() const { return m_data; }
    qint64 size() const { return m_size; }

private:
#ifdef _WIN32
    void* m_file;       // HANDLE; nullptr when closed.
    void* m_mapping;    // HANDLE; nullptr when closed.
#else
    int m_fd;           // -1 when closed.
#endif
    const char* m_data;
    qint64 m_size;
};

class OverlayManager
{
public:
    OverlayManager();
    ~OverlayManager();

    bool isOverlayEnabled(OverlayType type);
    char* getOverlayText(OverlayType type);
    void updateOverlayText(OverlayType type, const char* text);
    int getOverlayMaxTextLength();
    void setOverlayTextUpdated(OverlayType type);
    void setOverlayState(OverlayType type, bool enabled);
    SDL_Color getOverlayColor(OverlayType type);
    int getOverlayFontSize(OverlayType type);
    SDL_Surface* getUpdatedOverlaySurface(OverlayType type);

    void setOverlayRenderer(IOverlayRenderer* renderer);

private:
    void notifyOverlayUpdated(OverlayType type);

    struct {
        bool enabled;
        int fontSize;
        SDL_Color color;
        char text[2048];

        TTF_Font* font;
        SDL_Surface* surface;
    } m_Overlays[OverlayMax];
    IOverlayRenderer* m_Renderer;
    MappedFile m_MappedFont;        // primary: mmap'd system CJK font.
    QByteArray m_FallbackFont;      // fallback: embedded ModeSeven.ttf.
};

}

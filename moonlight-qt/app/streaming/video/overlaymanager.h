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
    // §REVIEW-1127 qt_stream #3：文字仍是 expected 才收掉（比對與收掉在同一把 slot 鎖裡）。
    // 共用 slot 的倒數收掉用這個，避免收到別的功能剛寫進去的提示。回傳是否收掉。
    bool hideOverlayIfText(OverlayType type, const char* expected);
    SDL_Color getOverlayColor(OverlayType type);
    int getOverlayFontSize(OverlayType type);
    SDL_Surface* getUpdatedOverlaySurface(OverlayType type);

    void setOverlayRenderer(IOverlayRenderer* renderer);

private:
    // §REVIEW-1127 qt_stream #3：原本的 notifyOverlayUpdated 拆成兩段：
    // 在 slot 鎖裡畫 surface（*Locked），鎖外才通知 renderer（避免和 renderer 自己的鎖交叉卡死）。
    bool buildOverlaySurfaceLocked(OverlayType type);
    bool setOverlayStateLocked(OverlayType type, bool enabled);
    void notifyRenderer(OverlayType type);

    struct {
        bool enabled;
        int fontSize;
        SDL_Color color;
        char text[2048];

        TTF_Font* font;
        SDL_Surface* surface;
    } m_Overlays[OverlayMax];
    // §REVIEW-1127 qt_stream #3：每個 slot 一把鎖（SDL_mutex 可重入），涵蓋寫文字、切換狀態與重畫 surface。
    // 同一個 slot 會被主、解碼、呈現執行緒同時寫；同一個 TTF_Font 不能兩條執行緒同時用。
    // 不共用一把：解碼執行緒每秒畫一次 debug HUD 要幾毫秒，不能擋住其他 slot。
    SDL_mutex* m_SlotLock[OverlayMax];
    // §REVIEW-1127 qt_stream #3：TTF_OpenFontRW 會動到共用的 FreeType library，兩個 slot 同時第一次開字型時要排隊
    SDL_mutex* m_FontOpenLock;
    IOverlayRenderer* m_Renderer;
    MappedFile m_MappedFont;        // primary: mmap'd system CJK font.
    QByteArray m_FallbackFont;      // fallback: embedded ModeSeven.ttf.
};

}

// VipleStream 2.0 §VR-PLAYER — 影片的畫面格式（投影＋立體排列）：判斷規則與名稱（純邏輯）。
//
// 頭盔影片模式要知道一支影片該怎麼擺：一般的平面大螢幕、3D 左右／上下並排、180° 或 360° 的全景。判斷順序：
//   1. 使用者指定（--vr-format 或介面上選的）——一律優先；
//   2. 容器裡的中繼資料（FFmpeg 的 AV_PKT_DATA_SPHERICAL／AV_PKT_DATA_STEREO3D，由呼叫端讀出來填進 Hints）；
//   3. 檔名上的標記（`_360`、`_180`、`_SBS`、`_LR`、`_TB`、`_OU`、`_3dh`…，以非英數字元分隔、不分大小寫）；
//   4. 都沒有＝平面、單眼。
// 180° 沒有立體標記時當成左右並排（VR180 幾乎都是立體的）；360° 沒有標記時當成單眼。
//
// 「半寬／全寬」並排不在這裡判斷：呈現時看整張的長寬比（左右並排的整張比 2.5:1 還寬＝全寬，每一眼用半張的比例；
// 否則＝半寬，每一眼還原成整張的比例），見 eyeAspect()。
//
// header-only，只用標準函式庫（不依賴 Qt／FFmpeg／OpenXR），做法同 xrflight.h：`xr-probe --selftest-latch` 會接著跑這裡的
// selfTest()。

#pragma once

#include <cctype>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace playerfmt {

// Eac360＝等角立方體（YouTube 的 360° 影片用這種：3×2 六個面，下排轉 90°，排法同 FFmpeg v360 的 eac）
enum class Projection { Flat = 0, Equirect180 = 1, Equirect360 = 2, Eac360 = 3 };
enum class Stereo { Mono = 0, LeftRight = 1, TopBottom = 2 };

struct Format {
    Projection projection = Projection::Flat;
    Stereo stereo = Stereo::Mono;
    bool operator==(const Format& o) const { return projection == o.projection && stereo == o.stereo; }
    bool operator!=(const Format& o) const { return !(*this == o); }
};

// 從哪裡判斷出來的（給 log 與介面看）
enum class Source { User = 0, Metadata = 1, FileName = 2, Default = 3 };

// 呼叫端從容器讀出的中繼資料（沒有就留 false）
struct Hints {
    bool haveSpherical = false;
    bool sphericalEquirect = false;  // AV_SPHERICAL_EQUIRECTANGULAR（含 tile 版本）
    bool spherical180 = false;       // equirect 的左右邊界裁掉約一半（bound_left＋bound_right ≈ 0.5）
    bool haveStereo = false;
    Stereo stereo = Stereo::Mono;    // AV_STEREO3D_SIDEBYSIDE→LeftRight、TOPBOTTOM→TopBottom、2D→Mono
};

// 正規名稱（CLI、設定、log 共用）：flat、flat-sbs、flat-tb、180-mono、180-sbs、180-tb、360-mono、360-sbs、360-tb、
// 360eac-mono、360eac-sbs、360eac-tb
inline std::string name(const Format& f)
{
    const char* p = f.projection == Projection::Flat          ? "flat"
                    : f.projection == Projection::Equirect180 ? "180"
                    : f.projection == Projection::Equirect360 ? "360"
                                                              : "360eac";
    const char* s = f.stereo == Stereo::Mono ? "mono" : (f.stereo == Stereo::LeftRight ? "sbs" : "tb");
    if (f.projection == Projection::Flat) {
        return f.stereo == Stereo::Mono ? std::string("flat") : std::string("flat-") + s;
    }
    return std::string(p) + "-" + s;
}

inline const char* sourceName(Source s)
{
    switch (s) {
    case Source::User: return "user";
    case Source::Metadata: return "metadata";
    case Source::FileName: return "filename";
    default: return "default";
    }
}

inline std::string lower(std::string s)
{
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

// 名稱 → 格式。也接受常見的別名（2d、sbs、lr、tb、ou、180、360、vr180…）；"auto" 與不認得的回 false
inline bool parse(const std::string& text, Format* out)
{
    const std::string t = lower(text);
    struct Alias {
        const char* n;
        Projection p;
        Stereo s;
    };
    static const Alias kAliases[] = {
        {"flat", Projection::Flat, Stereo::Mono},          {"2d", Projection::Flat, Stereo::Mono},
        {"flat-mono", Projection::Flat, Stereo::Mono},     {"flat-sbs", Projection::Flat, Stereo::LeftRight},
        {"sbs", Projection::Flat, Stereo::LeftRight},      {"lr", Projection::Flat, Stereo::LeftRight},
        {"3d-sbs", Projection::Flat, Stereo::LeftRight},   {"flat-tb", Projection::Flat, Stereo::TopBottom},
        {"tb", Projection::Flat, Stereo::TopBottom},       {"ou", Projection::Flat, Stereo::TopBottom},
        {"3d-tb", Projection::Flat, Stereo::TopBottom},    {"180", Projection::Equirect180, Stereo::LeftRight},
        {"vr180", Projection::Equirect180, Stereo::LeftRight}, {"180-mono", Projection::Equirect180, Stereo::Mono},
        {"180-sbs", Projection::Equirect180, Stereo::LeftRight}, {"180-tb", Projection::Equirect180, Stereo::TopBottom},
        {"360", Projection::Equirect360, Stereo::Mono},    {"360-mono", Projection::Equirect360, Stereo::Mono},
        {"360-sbs", Projection::Equirect360, Stereo::LeftRight}, {"360-tb", Projection::Equirect360, Stereo::TopBottom},
        {"eac", Projection::Eac360, Stereo::Mono},         {"360-eac", Projection::Eac360, Stereo::Mono},
        {"360eac-mono", Projection::Eac360, Stereo::Mono}, {"360eac-sbs", Projection::Eac360, Stereo::LeftRight},
        {"360eac-tb", Projection::Eac360, Stereo::TopBottom},
    };
    for (const Alias& a : kAliases) {
        if (t == a.n) {
            out->projection = a.p;
            out->stereo = a.s;
            return true;
        }
    }
    return false;
}

// 檔名（或網址的路徑最後一段）裡的標記。回傳 true＝至少認出一個標記
inline bool fromFileName(const std::string& fileName, Format* out)
{
    // 只看最後一段、去掉副檔名與查詢字串
    std::string base = fileName;
    const size_t q = base.find_first_of("?#");
    if (q != std::string::npos) {
        base.resize(q);
    }
    const size_t slash = base.find_last_of("/\\");
    if (slash != std::string::npos) {
        base = base.substr(slash + 1);
    }
    const size_t dot = base.find_last_of('.');
    if (dot != std::string::npos && dot > 0) {
        base.resize(dot);
    }
    base = lower(base);
    std::vector<std::string> tokens;
    std::string cur;
    for (char c : base) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            cur += c;
        }
        else if (!cur.empty()) {
            tokens.push_back(cur);
            cur.clear();
        }
    }
    if (!cur.empty()) {
        tokens.push_back(cur);
    }
    bool is180 = false, is360 = false, lr = false, tb = false, mono = false, eac = false;
    for (const std::string& k : tokens) {
        if (k == "360" || k == "360vr" || k == "vr360" || k == "360x180" || k == "equirect" || k == "mono360") {
            is360 = true;
            if (k == "mono360") {
                mono = true;
            }
        }
        else if (k == "180" || k == "vr180" || k == "180vr" || k == "180x180" || k == "fisheye180") {
            is180 = true;
        }
        else if (k == "sbs" || k == "lr" || k == "3dh" || k == "hsbs" || k == "fsbs" || k == "halfsbs" || k == "fullsbs" ||
                 k == "sidebyside" || k == "leftright" || k == "stereo") {
            lr = true;
        }
        else if (k == "tb" || k == "ou" || k == "3dv" || k == "tab" || k == "htab" || k == "hou" || k == "overunder" ||
                 k == "topbottom" || k == "halftab" || k == "fulltab") {
            tb = true;
        }
        else if (k == "mono" || k == "2d") {
            mono = true;
        }
        else if (k == "eac") {
            eac = true;
            is360 = true;
        }
        // 常見的黏在一起寫法：「360tb」「180sbs」「360lr」
        else if (k.size() > 3 && (k.compare(0, 3, "360") == 0 || k.compare(0, 3, "180") == 0)) {
            const std::string rest = k.substr(3);
            const bool r360 = k.compare(0, 3, "360") == 0;
            if (rest == "tb" || rest == "ou" || rest == "sbs" || rest == "lr" || rest == "mono") {
                (r360 ? is360 : is180) = true;
                if (rest == "tb" || rest == "ou") {
                    tb = true;
                }
                else if (rest == "mono") {
                    mono = true;
                }
                else {
                    lr = true;
                }
            }
        }
    }
    if (!is180 && !is360 && !lr && !tb) {
        return false;
    }
    Format f;
    f.projection = is360 ? (eac ? Projection::Eac360 : Projection::Equirect360) : (is180 ? Projection::Equirect180 : Projection::Flat);
    if (tb) {
        f.stereo = Stereo::TopBottom;
    }
    else if (lr) {
        f.stereo = Stereo::LeftRight;
    }
    else if (mono) {
        f.stereo = Stereo::Mono;
    }
    else {
        // 只有投影的標記：180° 預設立體左右、360° 預設單眼
        f.stereo = f.projection == Projection::Equirect180 ? Stereo::LeftRight : Stereo::Mono;
    }
    *out = f;
    return true;
}

// 中繼資料 → 格式。回傳 true＝中繼資料有說
inline bool fromHints(const Hints& h, Format* out)
{
    // 只寫了「單眼」而沒有全景資訊的不算數：很多檔案（例如 YouTube 的 webm）一律帶 StereoMode＝mono，360° 的片也是，
    // 拿它決定就永遠是平面，檔名或片名的標記用不上
    const bool stereoSays = h.haveStereo && h.stereo != Stereo::Mono;
    if (!h.haveSpherical && !stereoSays) {
        return false;
    }
    Format f;
    if (h.haveSpherical && h.sphericalEquirect) {
        f.projection = h.spherical180 ? Projection::Equirect180 : Projection::Equirect360;
    }
    if (h.haveStereo) {
        f.stereo = h.stereo;
    }
    else if (f.projection == Projection::Equirect180) {
        f.stereo = Stereo::LeftRight;
    }
    *out = f;
    return true;
}

// 依上面的順序決定。userText 空字串或 "auto"＝沒指定
inline Format decide(const std::string& userText, const Hints& hints, const std::string& fileName, Source* source)
{
    Format f;
    if (!userText.empty() && lower(userText) != "auto" && parse(userText, &f)) {
        *source = Source::User;
        return f;
    }
    if (fromHints(hints, &f)) {
        *source = Source::Metadata;
        return f;
    }
    if (fromFileName(fileName, &f)) {
        *source = Source::FileName;
        return f;
    }
    *source = Source::Default;
    return Format();
}

// 每一眼看到的畫面比例（寬／高）。frameAspect＝整張解碼影像的顯示比例（已含 SAR）
inline float eyeAspect(const Format& f, float frameAspect)
{
    if (frameAspect <= 0.0f) {
        return 16.0f / 9.0f;
    }
    if (f.stereo == Stereo::LeftRight) {
        // 全寬並排（例如 3840x1080）每一眼是半張；半寬並排（1920x1080）每一眼要還原成整張的比例
        return frameAspect > 2.5f ? frameAspect * 0.5f : frameAspect;
    }
    if (f.stereo == Stereo::TopBottom) {
        // 全高上下（1920x2160）每一眼是半張；半高上下（1920x1080）還原成整張
        return frameAspect < 1.2f ? frameAspect * 2.0f : frameAspect;
    }
    return frameAspect;
}

// 第 eye 眼（0 左、1 右）在整張影像裡的範圍（0..1，左上原點）：x0、y0、寬、高
inline void eyeRect(const Format& f, int eye, float r[4])
{
    r[0] = 0.0f;
    r[1] = 0.0f;
    r[2] = 1.0f;
    r[3] = 1.0f;
    if (f.stereo == Stereo::LeftRight) {
        r[0] = eye == 0 ? 0.0f : 0.5f;
        r[2] = 0.5f;
    }
    else if (f.stereo == Stereo::TopBottom) {
        r[1] = eye == 0 ? 0.0f : 0.5f;
        r[3] = 0.5f;
    }
}

// 自測；回傳失敗數。每個案例呼叫 report(名稱, 是否通過, 細節)（簽名同 xrlatch::selfTest）
inline int selfTest(const std::function<void(const char* name, bool ok, const std::string& detail)>& report)
{
    int failures = 0;
    auto check = [&](const char* nm, bool ok, const std::string& detail) {
        if (!ok) {
            failures++;
        }
        report(nm, ok, detail);
    };
    struct NameCase {
        const char* file;
        const char* want;  // nullptr＝沒有標記
    };
    static const NameCase kNames[] = {
        {"/home/u/Videos/Trip_360.mp4", "360-mono"},
        {"concert_180_LR.mkv", "180-sbs"},
        {"Avatar.2009.1080p.3D.HSBS.mkv", "flat-sbs"},
        {"movie-3D-HOU.mkv", "flat-tb"},
        {"https://x.example/v/clip_360_TB.mp4?sig=1#t", "360-tb"},
        {"vr180 beach.mp4", "180-sbs"},
        {"dive_360tb_8k.mp4", "360-tb"},
        {"room_180_mono.mp4", "180-mono"},
        {"Holiday 2024.mp4", nullptr},
        {"report_v1.80.mp4", nullptr},
        {"sbs.mp4", "flat-sbs"},
    };
    int idx = 0;
    for (const NameCase& c : kNames) {
        Format f;
        const bool got = fromFileName(c.file, &f);
        const std::string want = c.want != nullptr ? c.want : "-";
        const std::string have = got ? name(f) : "-";
        char nm[48];
        std::snprintf(nm, sizeof(nm), "playerfmt-name-%d", idx);
        check(nm, have == want, std::string(c.file) + " -> " + have + " (want " + want + ")");
        idx++;
    }
    {
        // 名稱來回
        bool ok = true;
        std::string bad;
        for (int p = 0; p < 4; p++) {
            for (int s = 0; s < 3; s++) {
                Format f;
                f.projection = static_cast<Projection>(p);
                f.stereo = static_cast<Stereo>(s);
                Format g;
                if (!parse(name(f), &g) || g != f) {
                    ok = false;
                    bad += name(f) + " ";
                }
            }
        }
        Format g;
        ok = ok && !parse("auto", &g) && !parse("bogus", &g);
        check("playerfmt-name-roundtrip", ok, bad.empty() ? "ok" : bad);
    }
    {
        // 優先順序：使用者 > 中繼資料 > 檔名 > 預設
        Hints h;
        h.haveSpherical = true;
        h.sphericalEquirect = true;
        Source s1, s2, s3, s4;
        const Format a = decide("flat", h, "x_180_LR.mp4", &s1);
        const Format b = decide("auto", h, "x_180_LR.mp4", &s2);
        const Format c = decide("", Hints(), "x_180_LR.mp4", &s3);
        const Format d = decide("", Hints(), "x.mp4", &s4);
        const bool ok = name(a) == "flat" && s1 == Source::User && name(b) == "360-mono" && s2 == Source::Metadata &&
                        name(c) == "180-sbs" && s3 == Source::FileName && name(d) == "flat" && s4 == Source::Default;
        check("playerfmt-priority", ok, name(a) + "/" + name(b) + "/" + name(c) + "/" + name(d));
    }
    {
        // 中繼資料：180（裁邊）預設立體左右、立體資訊單獨出現
        Hints h;
        h.haveSpherical = true;
        h.sphericalEquirect = true;
        h.spherical180 = true;
        Format f;
        Hints st;
        st.haveStereo = true;
        st.stereo = Stereo::TopBottom;
        Format g;
        Hints mono;  // 只寫單眼、沒有全景資訊：不算數（交給檔名）
        mono.haveStereo = true;
        Format m;
        Source src;
        const Format byName = decide("", mono, "Lions 360 National Geographic", &src);
        const bool ok = fromHints(h, &f) && name(f) == "180-sbs" && fromHints(st, &g) && name(g) == "flat-tb" &&
                        !fromHints(mono, &m) && name(byName) == "360-mono" && src == Source::FileName;
        check("playerfmt-hints", ok, name(f) + "/" + name(g) + "/" + name(byName));
    }
    {
        // 每眼比例與範圍：半寬／全寬並排、半高／全高上下
        Format sbs;
        sbs.stereo = Stereo::LeftRight;
        Format tb;
        tb.stereo = Stereo::TopBottom;
        const float hs = eyeAspect(sbs, 1920.0f / 1080.0f), fs = eyeAspect(sbs, 3840.0f / 1080.0f);
        const float ht = eyeAspect(tb, 1920.0f / 1080.0f), ft = eyeAspect(tb, 1920.0f / 2160.0f);
        float r0[4], r1[4];
        eyeRect(tb, 1, r1);
        eyeRect(sbs, 0, r0);
        auto approx = [](float a, float b) { return a - b < 0.01f && b - a < 0.01f; };
        const bool ok = approx(hs, 1.778f) && approx(fs, 1.778f) && approx(ht, 1.778f) && approx(ft, 1.778f) && r1[1] == 0.5f &&
                        r1[3] == 0.5f && r0[0] == 0.0f && r0[2] == 0.5f;
        char buf[96];
        std::snprintf(buf, sizeof(buf), "hsbs=%.3f fsbs=%.3f htb=%.3f ftb=%.3f", static_cast<double>(hs),
                      static_cast<double>(fs), static_cast<double>(ht), static_cast<double>(ft));
        check("playerfmt-eye", ok, buf);
    }
    return failures;
}

}  // namespace playerfmt

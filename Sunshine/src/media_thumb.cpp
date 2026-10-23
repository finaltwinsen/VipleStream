/**
 * @file src/media_thumb.cpp
 * @brief VipleStream §VR-PLAYER-THUMB（2026-10-23）：影片模式「電腦」分頁的縮圖，由 host 產生（/viple/media/thumb）。
 *
 * server 連結的 FFmpeg 只有編碼用的 libavcodec／libswscale／libavutil，沒有 libavformat，拆不開 mp4／mkv，
 * 所以改用作業系統本來就有的縮圖：
 *   - Windows：Shell 的 IShellItemImageFactory（檔案總管用的同一套，系統自己快取），WIC 縮成寬 320 的 JPEG。
 *     server 行程跑在使用者的工作階段（不是 Session 0），拿得到使用者的縮圖快取與解碼器。
 *   - Linux：先找 freedesktop 縮圖快取（~/.cache/thumbnails），沒有就呼叫 ffmpegthumbnailer 或 ffmpeg（有裝才有）。
 * 都拿不到時回 false（端點回 404，client 顯示預設圖示）。一次只做一張（排隊），單張最多等 8 秒。
 */
#include "media_share.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "logging.h"

#ifdef _WIN32
  #include <windows.h>
  #include <shlwapi.h>
  #include <shobjidl.h>
  #include <wincodec.h>
#elif defined(__linux__)
  #include <cstdlib>
  #include <unistd.h>

  #include <boost/process/v1.hpp>
  #include <boost/process/v1/args.hpp>
  #include <boost/process/v1/child.hpp>
  #include <boost/process/v1/io.hpp>
  #include <boost/process/v1/search_path.hpp>
  #include <openssl/evp.h>
#endif

namespace fs = std::filesystem;
using namespace std::literals;

namespace media_share {

  namespace {
    constexpr int k_thumb_width = 320;
    constexpr auto k_thumb_timeout = 8s;

    struct thumb_result_t {
      bool ok = false;
      std::string data;
      std::string content_type;
      std::string error;
    };

#ifdef _WIN32
    // 最小的 COM 指標（不依賴 WRL；MinGW 與 MSVC 都能編）
    template<class T>
    struct com_ptr {
      T *p = nullptr;
      com_ptr() = default;
      com_ptr(const com_ptr &) = delete;
      com_ptr &operator=(const com_ptr &) = delete;
      ~com_ptr() {
        if (p) {
          p->Release();
        }
      }
      T **put() {
        return &p;
      }
      T *operator->() const {
        return p;
      }
      T *get() const {
        return p;
      }
    };

    thumb_result_t shell_thumbnail(const fs::path &path) {
      thumb_result_t r;
      const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
      HBITMAP hbmp = nullptr;
      auto fail = [&](const char *what, HRESULT hr) {
        char buf[96];
        snprintf(buf, sizeof(buf), "%s failed (0x%08lx)", what, static_cast<unsigned long>(hr));
        r.error = buf;
      };
      do {
        com_ptr<IShellItemImageFactory> factory;
        HRESULT hr = SHCreateItemFromParsingName(path.wstring().c_str(), nullptr, IID_PPV_ARGS(factory.put()));
        if (FAILED(hr)) {
          fail("SHCreateItemFromParsingName", hr);
          break;
        }
        // THUMBNAILONLY：沒有縮圖時失敗，不要拿檔案類型的圖示充數
        hr = factory->GetImage(SIZE {k_thumb_width, k_thumb_width}, SIIGBF_THUMBNAILONLY | SIIGBF_BIGGERSIZEOK, &hbmp);
        if (FAILED(hr) || hbmp == nullptr) {
          fail("GetImage", hr);
          break;
        }
        com_ptr<IWICImagingFactory> wic;
        hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic.put()));
        if (FAILED(hr)) {
          fail("WIC factory", hr);
          break;
        }
        com_ptr<IWICBitmap> bmp;
        hr = wic->CreateBitmapFromHBITMAP(hbmp, nullptr, WICBitmapIgnoreAlpha, bmp.put());
        if (FAILED(hr)) {
          fail("CreateBitmapFromHBITMAP", hr);
          break;
        }
        UINT w = 0, h = 0;
        bmp->GetSize(&w, &h);
        if (w == 0 || h == 0) {
          r.error = "empty thumbnail";
          break;
        }
        IWICBitmapSource *src = bmp.get();
        com_ptr<IWICBitmapScaler> scaler;
        if (w > static_cast<UINT>(k_thumb_width)) {
          const UINT th = std::max<UINT>(1, h * k_thumb_width / w);
          if (SUCCEEDED(wic->CreateBitmapScaler(scaler.put())) &&
              SUCCEEDED(scaler->Initialize(bmp.get(), k_thumb_width, th, WICBitmapInterpolationModeFant))) {
            src = scaler.get();
          }
        }
        com_ptr<IWICFormatConverter> conv;
        hr = wic->CreateFormatConverter(conv.put());
        if (SUCCEEDED(hr)) {
          hr = conv->Initialize(src, GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
        }
        if (FAILED(hr)) {
          fail("format converter", hr);
          break;
        }
        com_ptr<IStream> stream;
        stream.p = SHCreateMemStream(nullptr, 0);
        if (stream.p == nullptr) {
          r.error = "SHCreateMemStream failed";
          break;
        }
        com_ptr<IWICBitmapEncoder> enc;
        com_ptr<IWICBitmapFrameEncode> frame;
        com_ptr<IPropertyBag2> props;
        hr = wic->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, enc.put());
        if (SUCCEEDED(hr)) {
          hr = enc->Initialize(stream.get(), WICBitmapEncoderNoCache);
        }
        if (SUCCEEDED(hr)) {
          hr = enc->CreateNewFrame(frame.put(), props.put());
        }
        if (SUCCEEDED(hr) && props.get() != nullptr) {
          PROPBAG2 opt = {};
          opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
          VARIANT v;
          VariantInit(&v);
          v.vt = VT_R4;
          v.fltVal = 0.8f;
          props->Write(1, &opt, &v);  // 失敗就用預設品質
        }
        UINT ow = 0, oh = 0;
        conv->GetSize(&ow, &oh);
        WICPixelFormatGUID pf = GUID_WICPixelFormat24bppBGR;
        if (SUCCEEDED(hr)) {
          hr = frame->Initialize(props.get());
        }
        if (SUCCEEDED(hr)) {
          hr = frame->SetSize(ow, oh);
        }
        if (SUCCEEDED(hr)) {
          hr = frame->SetPixelFormat(&pf);
        }
        if (SUCCEEDED(hr)) {
          hr = frame->WriteSource(conv.get(), nullptr);
        }
        if (SUCCEEDED(hr)) {
          hr = frame->Commit();
        }
        if (SUCCEEDED(hr)) {
          hr = enc->Commit();
        }
        if (FAILED(hr)) {
          fail("JPEG encode", hr);
          break;
        }
        STATSTG st = {};
        if (FAILED(stream->Stat(&st, STATFLAG_NONAME)) || st.cbSize.QuadPart == 0 || st.cbSize.QuadPart > 8 * 1024 * 1024) {
          r.error = "bad encoded size";
          break;
        }
        LARGE_INTEGER zero = {};
        stream->Seek(zero, STREAM_SEEK_SET, nullptr);
        r.data.resize(static_cast<size_t>(st.cbSize.QuadPart));
        ULONG got = 0;
        if (FAILED(stream->Read(r.data.data(), static_cast<ULONG>(r.data.size()), &got)) || got != r.data.size()) {
          r.error = "read encoded data failed";
          r.data.clear();
          break;
        }
        r.content_type = "image/jpeg";
        r.ok = true;
      } while (false);
      if (hbmp != nullptr) {
        DeleteObject(hbmp);
      }
      if (SUCCEEDED(init)) {
        CoUninitialize();
      }
      return r;
    }
#elif defined(__linux__)
    bool read_file(const fs::path &p, std::string *out) {
      std::ifstream in(p, std::ios::binary);
      if (!in) {
        return false;
      }
      out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
      return !out->empty();
    }

    // freedesktop 縮圖規格：~/.cache/thumbnails/<大小>/<md5("file://" + 百分比編碼的絕對路徑)>.png
    bool freedesktop_thumbnail(const fs::path &path, thumb_result_t *r) {
      std::string uri = "file://";
      for (const unsigned char c : path.string()) {
        if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') {
          uri += static_cast<char>(c);
        }
        else {
          char buf[4];
          snprintf(buf, sizeof(buf), "%%%02X", c);
          uri += buf;
        }
      }
      unsigned char md[EVP_MAX_MD_SIZE];
      unsigned int mdlen = 0;
      if (EVP_Digest(uri.data(), uri.size(), md, &mdlen, EVP_md5(), nullptr) != 1) {
        return false;
      }
      std::string hex;
      for (unsigned int i = 0; i < mdlen; i++) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", md[i]);
        hex += buf;
      }
      fs::path base;
      if (const char *x = std::getenv("XDG_CACHE_HOME"); x != nullptr && *x != '\0') {
        base = x;
      }
      else if (const char *home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        base = fs::path(home) / ".cache";
      }
      else {
        return false;
      }
      for (const char *dir : {"large", "normal", "x-large", "xx-large"}) {
        if (read_file(base / "thumbnails" / dir / (hex + ".png"), &r->data)) {
          r->content_type = "image/png";
          r->ok = true;
          return true;
        }
      }
      return false;
    }

    bool run_tool(const fs::path &tool, const std::vector<std::string> &args, const fs::path &out, thumb_result_t *r) {
      namespace bp = boost::process::v1;
      std::error_code ec;
      fs::remove(out, ec);
      try {
        bp::child c(tool.string(), bp::args(args), bp::std_out > bp::null, bp::std_err > bp::null, bp::std_in < bp::null);
        if (!c.wait_for(std::chrono::duration_cast<std::chrono::milliseconds>(k_thumb_timeout))) {
          c.terminate();
          r->error = tool.filename().string() + " timed out";
          return false;
        }
      } catch (const std::exception &e) {
        r->error = e.what();
        return false;
      }
      const bool ok = read_file(out, &r->data);
      fs::remove(out, ec);
      if (ok) {
        r->content_type = "image/jpeg";
        r->ok = true;
      }
      return ok;
    }

    thumb_result_t linux_thumbnail(const fs::path &path) {
      namespace bp = boost::process::v1;
      thumb_result_t r;
      if (freedesktop_thumbnail(path, &r)) {
        return r;
      }
      static std::atomic<unsigned> counter {0};
      const fs::path out = fs::temp_directory_path() / ("viple-thumb-" + std::to_string(getpid()) + "-" + std::to_string(counter++) + ".jpg");
      const std::string w = std::to_string(k_thumb_width);
      if (const auto t = bp::search_path("ffmpegthumbnailer"); !t.empty()) {
        if (run_tool(t.string(), {"-i", path.string(), "-o", out.string(), "-c", "jpeg", "-s", w, "-q", "7"}, out, &r)) {
          return r;
        }
      }
      if (const auto t = bp::search_path("ffmpeg"); !t.empty()) {
        // 先跳到第 15 秒（片頭常是黑畫面），片子太短抓不到再從頭
        for (const char *ss : {"15", "0"}) {
          if (run_tool(t.string(), {"-v", "error", "-y", "-ss", ss, "-i", path.string(), "-frames:v", "1", "-vf", "scale=" + w + ":-2", "-q:v", "5", out.string()}, out, &r)) {
            return r;
          }
        }
      }
      if (r.error.empty()) {
        r.error = "no thumbnail cache entry and neither ffmpegthumbnailer nor ffmpeg is installed";
      }
      return r;
    }
#endif
  }  // namespace

  bool thumbnail(const fs::path &path, std::string *data, std::string *content_type, std::string *error) {
#if defined(_WIN32) || defined(__linux__)
    // 一次只做一張：頭盔一頁可能一口氣要幾十張，不要同時開幾十個解碼
    static std::mutex serial;
    std::lock_guard<std::mutex> lk(serial);
    // 在自己的執行緒做（Windows 要 STA；卡住時這條請求照樣在期限內回應，那條執行緒自己收尾）
    auto prom = std::make_shared<std::promise<thumb_result_t>>();
    auto fut = prom->get_future();
    std::thread([prom, path]() {
  #ifdef _WIN32
      prom->set_value(shell_thumbnail(path));
  #else
      prom->set_value(linux_thumbnail(path));
  #endif
    }).detach();
    if (fut.wait_for(k_thumb_timeout + 1s) != std::future_status::ready) {
      *error = "timed out";
      return false;
    }
    thumb_result_t r = fut.get();
    if (!r.ok) {
      *error = r.error;
      return false;
    }
    *data = std::move(r.data);
    *content_type = std::move(r.content_type);
    return true;
#else
    (void) path;
    (void) data;
    (void) content_type;
    *error = "thumbnails are not supported on this platform";
    return false;
#endif
  }

}  // namespace media_share

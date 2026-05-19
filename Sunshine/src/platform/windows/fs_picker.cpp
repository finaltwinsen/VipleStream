/**
 * @file src/platform/windows/fs_picker.cpp
 * @brief Windows native file picker (IFileOpenDialog / Common Item Dialog API)。
 */

#define WIN32_LEAN_AND_MEAN

#include "src/fs_picker.h"
#include "src/logging.h"

#include <windows.h>
#include <shlobj.h>      // SHGetKnownFolderPath, FOLDERID_Desktop
#include <shobjidl.h>
#include <shlwapi.h>
#include <wtsapi32.h>    // WTSGetActiveConsoleSessionId / WTSQueryUserToken

namespace fs_picker {

  namespace {

    /**
     * RAII：在 thread 上 init COM (apartment-threaded)，析構時 uninit。
     * IFileOpenDialog 必須在 STA 下用。Tray callback thread 通常 *未*
     * 初始化 COM，所以這個 wrapper 自己管。
     */
    struct com_init_t {
      HRESULT hr;
      explicit com_init_t() {
        hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
      }
      ~com_init_t() {
        if (SUCCEEDED(hr)) {
          ::CoUninitialize();
        }
      }
      bool ok() const {
        return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;  // 後者代表 thread 已 init 過 MTA，仍可用
      }
    };

    std::wstring utf8_to_wide(const std::string &s) {
      if (s.empty()) return {};
      int len = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
      if (len <= 0) return {};
      std::wstring w(static_cast<std::size_t>(len - 1), L'\0');
      ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), len);
      return w;
    }

    std::string wide_to_utf8(const std::wstring &w) {
      if (w.empty()) return {};
      int len = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
      if (len <= 0) return {};
      std::string s(static_cast<std::size_t>(len - 1), '\0');
      ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), len, nullptr, nullptr);
      return s;
    }

    /**
     * Resolve 互動使用者的 Desktop 路徑。Sunshine service 在 SYSTEM context
     * 下 SHGetKnownFolderPath(nullptr) 會回 systemprofile\Desktop（user 不
     * 能讀）。先抓 active console session 的 user token 用 user context 查。
     * 失敗回傳空（呼叫方就不 SetFolder，由 OS dialog 自己決定）。
     * 跟 file_transfer::manager::downloads_dir() 用同一個 pattern。
     */
    std::wstring active_user_desktop() {
      DWORD session_id = WTSGetActiveConsoleSessionId();
      if (session_id == 0xFFFFFFFF) {
        return {};
      }
      HANDLE user_tok = nullptr;
      if (!WTSQueryUserToken(session_id, &user_tok)) {
        BOOST_LOG(debug) << "[VIPLE-XFER] fs_picker: WTSQueryUserToken err=" << GetLastError();
        return {};
      }
      PWSTR path_w = nullptr;
      std::wstring out;
      if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, user_tok, &path_w)) && path_w) {
        out = path_w;
        CoTaskMemFree(path_w);
      }
      CloseHandle(user_tok);
      return out;
    }

  }  // namespace

  std::optional<std::filesystem::path> pick_open_file(const open_options &opts) {
    com_init_t com;
    if (!com.ok()) {
      BOOST_LOG(warning) << "[VIPLE-XFER] CoInitializeEx failed hr=0x" << std::hex << com.hr;
      return std::nullopt;
    }

    IFileOpenDialog *dlg = nullptr;
    HRESULT hr = ::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL,
                                    IID_IFileOpenDialog, reinterpret_cast<void **>(&dlg));
    if (FAILED(hr)) {
      BOOST_LOG(warning) << "[VIPLE-XFER] CoCreateInstance(FileOpenDialog) failed hr=0x" << std::hex << hr;
      return std::nullopt;
    }
    struct dlg_guard {
      IFileOpenDialog *p;
      ~dlg_guard() { if (p) p->Release(); }
    } guard {dlg};

    if (!opts.title.empty()) {
      auto wt = utf8_to_wide(opts.title);
      dlg->SetTitle(wt.c_str());
    }

    // 決定 starting folder：caller 指定 > 互動使用者 Desktop > OS 預設
    std::wstring start_dir;
    if (!opts.default_dir.empty()) {
      start_dir = utf8_to_wide(opts.default_dir);
    } else {
      start_dir = active_user_desktop();
    }
    if (!start_dir.empty()) {
      IShellItem *folder = nullptr;
      if (SUCCEEDED(::SHCreateItemFromParsingName(start_dir.c_str(), nullptr, IID_IShellItem,
                                                  reinterpret_cast<void **>(&folder)))) {
        dlg->SetFolder(folder);
        folder->Release();
      }
    }

    // 預設「single file」（不開資料夾、不允多選），符合 v1 規格
    DWORD opts_mask = 0;
    if (SUCCEEDED(dlg->GetOptions(&opts_mask))) {
      dlg->SetOptions(opts_mask | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
    }

    hr = dlg->Show(nullptr);
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
      return std::nullopt;
    }
    if (FAILED(hr)) {
      BOOST_LOG(warning) << "[VIPLE-XFER] IFileOpenDialog::Show failed hr=0x" << std::hex << hr;
      return std::nullopt;
    }

    IShellItem *result = nullptr;
    if (FAILED(dlg->GetResult(&result))) {
      return std::nullopt;
    }
    struct item_guard {
      IShellItem *p;
      ~item_guard() { if (p) p->Release(); }
    } iguard {result};

    PWSTR path_w = nullptr;
    if (FAILED(result->GetDisplayName(SIGDN_FILESYSPATH, &path_w)) || !path_w) {
      return std::nullopt;
    }
    std::wstring wpath = path_w;
    ::CoTaskMemFree(path_w);

    return std::filesystem::path(wpath);
  }

}  // namespace fs_picker

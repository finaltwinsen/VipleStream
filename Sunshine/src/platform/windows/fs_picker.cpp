/**
 * @file src/platform/windows/fs_picker.cpp
 * @brief Windows native file picker — spawn viple-fs-picker.exe helper into
 *        active user session (LocalSystem path) or in-proc fallback.
 *
 * §N.x 2026-05-26 — 由 viplestream-server.exe（LocalSystem context）直接
 * in-process 開 IFileOpenDialog 會踩到：
 *   - dialog 內部 shell32 worker thread 不繼承 main thread 的
 *     ImpersonateLoggedOnUser token，仍以 SYSTEM token 查 known-folder
 *     「Desktop」→ 解到 systemprofile\Desktop → shell verify 失敗
 *     → 跳「位置無法使用」警示窗。SetErrorMode SEM_FAILCRITICALERRORS
 *     不會被該 popup 抓到。
 *   - 主 thread impersonation 本身能讓 SetFolder 的 user path 正常顯示，
 *     但解決不了 worker thread 的 popup。
 *
 * 解：把整個 dialog 搬到獨立 helper 子程序 viple-fs-picker.exe，由本
 * fs_picker 透過 platf::run_command 的 CreateProcessAsUserW 路徑 spawn
 * 進 active console user 的 session。helper 整個 process token = user，
 * 所有 shell 內部 lookups 都解到 user 真實 profile。
 *
 * Helper IPC：argv 帶 --result <utf8 temp_path>，dialog 結果寫入該檔案
 * （UTF-8 path 或空字串 for cancel）。caller 等子程序結束後讀檔。
 */

#define WIN32_LEAN_AND_MEAN

#include "src/fs_picker.h"
#include "src/file_transfer.h"
#include "src/logging.h"
#include "src/platform/common.h"  // platf::run_command

#include <boost/process/v1.hpp>
#include <boost/process/v1/child.hpp>
#include <boost/process/v1/environment.hpp>
#include <boost/filesystem.hpp>

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>

#include <chrono>
#include <fstream>
#include <random>
#include <sstream>
#include <system_error>

namespace bp = boost::process::v1;

namespace fs_picker {

  namespace {

    std::wstring utf8_to_wide(const std::string &s) {
      if (s.empty()) return {};
      int len = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
      if (len <= 0) return {};
      std::wstring w(static_cast<std::size_t>(len - 1), L'\0');
      ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), len);
      return w;
    }

    /**
     * 找到跟本 process exe 同層的 viple-fs-picker.exe 絕對路徑。
     * 取不到回傳空（caller 走 in-proc fallback）。
     */
    std::wstring locate_helper_exe() {
      wchar_t mod[MAX_PATH] = {};
      DWORD n = ::GetModuleFileNameW(nullptr, mod, MAX_PATH);
      if (n == 0 || n >= MAX_PATH) {
        return {};
      }
      std::filesystem::path p {mod};
      p.replace_filename("viple-fs-picker.exe");
      if (!std::filesystem::exists(p)) {
        return {};
      }
      return p.wstring();
    }

    /**
     * 產生唯一 temp 檔路徑（UTF-8）：C:\Windows\Temp\viple-picker-<rand>.txt
     * SYSTEM 與 user 都讀得到的位置；不用 user TEMP 因為 SYSTEM 不一定能讀。
     */
    std::string make_temp_result_path() {
      static std::mt19937_64 rng {std::random_device {}()};
      std::ostringstream oss;
      oss << "C:\\Windows\\Temp\\viple-picker-"
          << ::GetCurrentProcessId() << "-"
          << std::hex << rng() << ".txt";
      return oss.str();
    }

    /**
     * 雙引號包裝單一 argv，假設內容不含 `"` 或 trailing `\`（對我們的
     * 用例足夠：title 是英文字 + 中文字，dir 是檔案系統路徑無 `"`）。
     */
    std::string quote_arg(const std::string &s) {
      return "\"" + s + "\"";
    }

    /**
     * 用 helper 子程序展開 dialog；阻塞等結果。回傳 path 或 nullopt。
     */
    std::optional<std::filesystem::path>
    pick_via_helper(const std::wstring &helper_exe_w, const open_options &opts) {
      std::string helper_exe_utf8;
      {
        int len = ::WideCharToMultiByte(CP_UTF8, 0, helper_exe_w.c_str(), -1,
                                        nullptr, 0, nullptr, nullptr);
        if (len > 0) {
          helper_exe_utf8.resize(static_cast<std::size_t>(len - 1));
          ::WideCharToMultiByte(CP_UTF8, 0, helper_exe_w.c_str(), -1,
                                helper_exe_utf8.data(), len, nullptr, nullptr);
        }
      }
      if (helper_exe_utf8.empty()) {
        return std::nullopt;
      }

      // 決定 starting folder：caller 指定 > 互動使用者 Downloads。
      std::string default_dir_utf8 = opts.default_dir;
      if (default_dir_utf8.empty()) {
        default_dir_utf8 = file_transfer::manager::downloads_dir().string();
      }

      std::string result_path = make_temp_result_path();

      std::string cmd = quote_arg(helper_exe_utf8) +
                        " --result " + quote_arg(result_path) +
                        " --title " + quote_arg(opts.title) +
                        " --dir " + quote_arg(default_dir_utf8);

      BOOST_LOG(info) << "[VIPLE-XFER] fs_picker: spawning helper "
                      << helper_exe_utf8 << " result=" << result_path;

      boost::filesystem::path work_dir = std::filesystem::path(helper_exe_w).parent_path().string();
      bp::environment env;  // empty — run_command merges user env for SYSTEM->user spawn
      std::error_code ec;

      // interactive=false → CREATE_NO_WINDOW（不要 console 黑窗）。
      // helper 已是 WIN32 subsystem，IFileOpenDialog 自己會建 GUI window，
      // 跟 console flag 無關。
      auto child = platf::run_command(false, false, cmd, work_dir, env, nullptr, ec, nullptr);
      if (ec) {
        BOOST_LOG(warning) << "[VIPLE-XFER] fs_picker: spawn failed: " << ec.message();
        ::DeleteFileA(result_path.c_str());
        return std::nullopt;
      }

      // 阻塞等子程序結束。helper 應該很快 (使用者按確定/取消)。
      child.wait();
      int rc = child.exit_code();

      // 讀 result 檔。helper 一進門就 truncate-write empty file，所以即使
      // helper 提早 crash 也不會讀到 stale 內容。
      std::string picked_path;
      {
        std::ifstream f(result_path, std::ios::binary);
        if (f) {
          std::ostringstream buf;
          buf << f.rdbuf();
          picked_path = buf.str();
        }
      }
      ::DeleteFileA(result_path.c_str());

      BOOST_LOG(info) << "[VIPLE-XFER] fs_picker: helper rc=" << rc
                      << " path_len=" << picked_path.size();

      if (rc != 0 || picked_path.empty()) {
        // rc=1 cancel; rc=2 error；統一 nullopt
        return std::nullopt;
      }

      // helper 寫 UTF-8 string；轉 wide path
      std::wstring wpath = utf8_to_wide(picked_path);
      if (wpath.empty()) {
        return std::nullopt;
      }
      return std::filesystem::path(wpath);
    }

    /**
     * In-process fallback：本程序 token 已是 user（非 SYSTEM）時直接開 dialog。
     * 也用於 helper exe 找不到的場景（cap 降級而非完全失敗）。
     */
    std::optional<std::filesystem::path>
    pick_in_process(const open_options &opts) {
      HRESULT hr_init = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
      bool com_owned = SUCCEEDED(hr_init);
      bool com_ok = com_owned || hr_init == RPC_E_CHANGED_MODE;
      if (!com_ok) {
        BOOST_LOG(warning) << "[VIPLE-XFER] CoInitializeEx failed hr=0x" << std::hex << hr_init;
        return std::nullopt;
      }

      auto co_cleanup = [&]() { if (com_owned) ::CoUninitialize(); };

      IFileOpenDialog *dlg = nullptr;
      HRESULT hr = ::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL,
                                      IID_IFileOpenDialog, reinterpret_cast<void **>(&dlg));
      if (FAILED(hr) || !dlg) {
        BOOST_LOG(warning) << "[VIPLE-XFER] CoCreateInstance(FileOpenDialog) failed hr=0x" << std::hex << hr;
        co_cleanup();
        return std::nullopt;
      }

      if (!opts.title.empty()) {
        auto wt = utf8_to_wide(opts.title);
        dlg->SetTitle(wt.c_str());
      }

      std::wstring start_dir;
      if (!opts.default_dir.empty()) {
        start_dir = utf8_to_wide(opts.default_dir);
      } else {
        start_dir = file_transfer::manager::downloads_dir().wstring();
      }
      if (!start_dir.empty()) {
        IShellItem *folder = nullptr;
        if (SUCCEEDED(::SHCreateItemFromParsingName(start_dir.c_str(), nullptr,
                                                    IID_IShellItem,
                                                    reinterpret_cast<void **>(&folder)))) {
          dlg->SetFolder(folder);
          folder->Release();
        }
      }

      DWORD opts_mask = 0;
      if (SUCCEEDED(dlg->GetOptions(&opts_mask))) {
        dlg->SetOptions(opts_mask | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
      }

      HRESULT show_hr = dlg->Show(nullptr);
      std::optional<std::filesystem::path> out;
      if (show_hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        // cancel: silent nullopt
      } else if (SUCCEEDED(show_hr)) {
        IShellItem *result = nullptr;
        if (SUCCEEDED(dlg->GetResult(&result)) && result) {
          PWSTR path_w = nullptr;
          if (SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &path_w)) && path_w) {
            out = std::filesystem::path(std::wstring(path_w));
            ::CoTaskMemFree(path_w);
          }
          result->Release();
        }
      } else {
        BOOST_LOG(warning) << "[VIPLE-XFER] IFileOpenDialog::Show failed hr=0x" << std::hex << show_hr;
      }

      dlg->Release();
      co_cleanup();
      return out;
    }

  }  // namespace

  std::optional<std::filesystem::path> pick_open_file(const open_options &opts) {
    // §N.x 2026-05-26 — 優先走 helper 子程序路徑（不論本 process 是不是 SYSTEM，
    // CreateProcessAsUserW 都會把 helper 重新搬到 active console user session
    // 內，給的 process token = user，徹底避開 LocalSystem shell32 worker thread
    // 解到 systemprofile\Desktop 的問題）。
    //
    // helper exe 不存在時 fallback 到 in-process dialog（會碰到 systemprofile
    // 警示窗，但至少 dialog 還能用）。
    std::wstring helper_exe = locate_helper_exe();
    if (!helper_exe.empty()) {
      auto picked = pick_via_helper(helper_exe, opts);
      if (picked) {
        return picked;
      }
      // helper rc != 0 — 可能取消，可能 error；統一回 nullopt 不再做 fallback
      // （fallback 在「使用者按取消」也會二次彈窗，不合常理）。
      return std::nullopt;
    }

    BOOST_LOG(warning) << "[VIPLE-XFER] fs_picker: helper exe not found, using in-process fallback";
    return pick_in_process(opts);
  }

}  // namespace fs_picker

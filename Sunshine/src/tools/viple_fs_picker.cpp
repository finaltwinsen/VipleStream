/**
 * @file src/tools/viple_fs_picker.cpp
 * @brief 獨立 helper exe：在使用者 session 內展開 Windows 原生 file picker。
 *
 * 由 viplestream-server.exe（LocalSystem context）透過 platf::run_command
 * 的 CreateProcessAsUserW 路徑 spawn 進 active console user 的 session，
 * 整個 process token 就是 user，所有 shell32 內部 known-folder lookups
 * （Desktop / Documents / Quick Access）都會解到 user 真實 profile，
 * 不會落到 systemprofile\Desktop 觸發「位置無法使用」警示窗。
 *
 * Args (wide-char via CommandLineToArgvW)：
 *   --result <temp_path>   選擇結果寫入此檔（UTF-8 string 或空字串）
 *   --title  <title>       對話框標題
 *   --dir    <default_dir> 預設起始資料夾
 *
 * Exit codes:
 *   0 = 使用者按確定，temp 檔內含 selected path
 *   1 = 使用者取消，temp 檔為空
 *   2 = 內部錯誤（COM init / dialog create 失敗）
 *
 * Subsystem = WIN32 (GUI)：以 wWinMain 為 entry point，避免出現 cmd
 * 黑色 console window。IFileOpenDialog 自己負責建 GUI window，所以
 * 使用者看到的是 native file picker；console 隱藏。IPC 用 temp 檔，
 * 不靠 stdin/stdout 重導。
 *
 * 跟 viple-splash.exe 同層級的工具：標準 lib (shell32/ole32/shlwapi/user32)，
 * 不 link 進 Sunshine 主程式，獨立 build 為 `viple-fs-picker.exe`。
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <objbase.h>
#include <shellapi.h>  // CommandLineToArgvW

#include <cstring>
#include <fstream>
#include <string>

namespace {

  std::string wide_to_utf8(const std::wstring &w) {
    if (w.empty()) return {};
    int len = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string s(static_cast<std::size_t>(len - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), len, nullptr, nullptr);
    return s;
  }

  /** UTF-8 → wide，用於把 --result 那個 utf8 路徑 open 成 wide ofstream。 */
  std::wstring utf8_to_wide(const std::string &s) {
    if (s.empty()) return {};
    int len = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (len <= 0) return {};
    std::wstring w(static_cast<std::size_t>(len - 1), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), len);
    return w;
  }

}  // namespace

extern "C" int APIENTRY wWinMain(
    [[maybe_unused]] HINSTANCE hInstance,
    [[maybe_unused]] HINSTANCE hPrevInstance,
    [[maybe_unused]] LPWSTR lpCmdLine,
    [[maybe_unused]] int nCmdShow) {

  // wWinMain 的 lpCmdLine 不含 exe 名稱；想要完整 argv 還是用
  // CommandLineToArgvW 拆 GetCommandLineW() 比較標準。
  int argc = 0;
  LPWSTR *argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
  if (!argv) {
    return 2;
  }

  std::wstring result_path_w;
  std::wstring title_w;
  std::wstring default_dir_w;

  for (int i = 1; i + 1 < argc; i++) {
    if (std::wcscmp(argv[i], L"--result") == 0) {
      result_path_w = argv[++i];
    } else if (std::wcscmp(argv[i], L"--title") == 0) {
      title_w = argv[++i];
    } else if (std::wcscmp(argv[i], L"--dir") == 0) {
      default_dir_w = argv[++i];
    }
  }
  ::LocalFree(argv);

  if (result_path_w.empty()) {
    // 沒 --result 無從回報，直接 exit。caller 應永遠帶這個 arg。
    return 2;
  }

  // 一律先建一個空的 result 檔，確保 caller 沒拿到舊資料；後續視 dialog
  // 結果 truncate-write 內容（成功）或 leave-empty（取消）。
  // ofstream 在 MSVC 上接受 wchar_t* 但 g++ 上需要 std::filesystem::path 或
  // 走 utf8 narrow。result_path 由 caller 產生（C:\Windows\Temp\...）是 ASCII，
  // narrow 轉換安全；統一走 utf8 比較跨編譯器。
  std::string result_path_utf8 = wide_to_utf8(result_path_w);
  auto write_result = [&](const std::string &content) {
    std::ofstream f(result_path_utf8, std::ios::binary | std::ios::trunc);
    if (f) {
      f.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
  };
  write_result("");

  HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  bool com_owned = SUCCEEDED(hr);
  if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
    return 2;
  }
  auto com_cleanup = [&]() { if (com_owned) ::CoUninitialize(); };

  IFileOpenDialog *dlg = nullptr;
  HRESULT hr_create = ::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL,
                                         IID_IFileOpenDialog, reinterpret_cast<void **>(&dlg));
  if (FAILED(hr_create) || !dlg) {
    com_cleanup();
    return 2;
  }

  if (!title_w.empty()) {
    dlg->SetTitle(title_w.c_str());
  }

  if (!default_dir_w.empty()) {
    IShellItem *folder = nullptr;
    if (SUCCEEDED(::SHCreateItemFromParsingName(default_dir_w.c_str(), nullptr,
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

  int exit_code = 2;
  if (show_hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
    exit_code = 1;
  } else if (SUCCEEDED(show_hr)) {
    IShellItem *result = nullptr;
    if (SUCCEEDED(dlg->GetResult(&result)) && result) {
      PWSTR path_w = nullptr;
      if (SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &path_w)) && path_w) {
        write_result(wide_to_utf8(path_w));
        ::CoTaskMemFree(path_w);
        exit_code = 0;
      }
      result->Release();
    }
  }

  dlg->Release();
  com_cleanup();
  return exit_code;
}

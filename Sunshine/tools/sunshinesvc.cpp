/**
 * @file tools/sunshinesvc.cpp
 * @brief Handles launching Sunshine.exe into user sessions as SYSTEM
 */
#define WIN32_LEAN_AND_MEAN
#include <format>
#include <string>
#include <Windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <WtsApi32.h>

// PROC_THREAD_ATTRIBUTE_JOB_LIST is currently missing from MinGW headers
#ifndef PROC_THREAD_ATTRIBUTE_JOB_LIST
  #define PROC_THREAD_ATTRIBUTE_JOB_LIST ProcThreadAttributeValue(13, FALSE, TRUE, FALSE)
#endif

SERVICE_STATUS_HANDLE service_status_handle;
SERVICE_STATUS service_status;
HANDLE stop_event;
HANDLE session_change_event;

constexpr auto SERVICE_NAME = "SunshineService";

DWORD WINAPI HandlerEx(DWORD dwControl, DWORD dwEventType, LPVOID lpEventData, LPVOID lpContext) {
  switch (dwControl) {
    case SERVICE_CONTROL_INTERROGATE:
      return NO_ERROR;

    case SERVICE_CONTROL_SESSIONCHANGE:
      // If a new session connects to the console, restart Sunshine
      // to allow it to spawn inside the new console session.
      if (dwEventType == WTS_CONSOLE_CONNECT) {
        SetEvent(session_change_event);
      }
      return NO_ERROR;

    case SERVICE_CONTROL_PRESHUTDOWN:
      // The system is shutting down
    case SERVICE_CONTROL_STOP:
      // Let SCM know we're stopping in up to 30 seconds
      service_status.dwCurrentState = SERVICE_STOP_PENDING;
      service_status.dwControlsAccepted = 0;
      service_status.dwWaitHint = 30 * 1000;
      SetServiceStatus(service_status_handle, &service_status);

      // Trigger ServiceMain() to start cleanup
      SetEvent(stop_event);
      return NO_ERROR;

    default:
      return ERROR_CALL_NOT_IMPLEMENTED;
  }
}

HANDLE CreateJobObjectForChildProcess() {
  HANDLE job_handle = CreateJobObjectW(nullptr, nullptr);
  if (!job_handle) {
    return nullptr;
  }

  JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_limit_info = {};

  // Kill Sunshine.exe when the final job object handle is closed (which will happen if we terminate unexpectedly).
  // This ensures we don't leave an orphaned Sunshine.exe running with an inherited handle to our log file.
  job_limit_info.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

  // Allow Sunshine.exe to use CREATE_BREAKAWAY_FROM_JOB when spawning processes to ensure they can to live beyond
  // the lifetime of SunshineSvc.exe. This avoids unexpected user data loss if we crash or are killed.
  job_limit_info.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_BREAKAWAY_OK;

  if (!SetInformationJobObject(job_handle, JobObjectExtendedLimitInformation, &job_limit_info, sizeof(job_limit_info))) {
    CloseHandle(job_handle);
    return nullptr;
  }

  return job_handle;
}

LPPROC_THREAD_ATTRIBUTE_LIST AllocateProcThreadAttributeList(DWORD attribute_count) {
  SIZE_T size;
  InitializeProcThreadAttributeList(nullptr, attribute_count, 0, &size);

  auto list = (LPPROC_THREAD_ATTRIBUTE_LIST) HeapAlloc(GetProcessHeap(), 0, size);
  if (list == nullptr) {
    return nullptr;
  }

  if (!InitializeProcThreadAttributeList(list, attribute_count, 0, &size)) {
    HeapFree(GetProcessHeap(), 0, list);
    return nullptr;
  }

  return list;
}

HANDLE DuplicateTokenForSession(DWORD console_session_id) {
  HANDLE current_token;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE, &current_token)) {
    return nullptr;
  }

  // Duplicate our own LocalSystem token
  HANDLE new_token;
  if (!DuplicateTokenEx(current_token, TOKEN_ALL_ACCESS, nullptr, SecurityImpersonation, TokenPrimary, &new_token)) {
    CloseHandle(current_token);
    return nullptr;
  }

  CloseHandle(current_token);

  // Change the duplicated token to the console session ID
  if (!SetTokenInformation(new_token, TokenSessionId, &console_session_id, sizeof(console_session_id))) {
    CloseHandle(new_token);
    return nullptr;
  }

  return new_token;
}

// VipleStream §S1-02：安裝目錄（本 exe 在 <install>\tools\，main() 去掉兩層路徑後記在這裡）
static std::wstring g_install_dir;

// VipleStream §S1-02 [VIPLE-SEC]：viplestream-svc.log 的安全描述元。protected DACL 只有 SYSTEM 與
// Administrators（沒有 Users），與 server 端 src/platform/windows/config_acl.cpp 的 kSddlFull 相同。
constexpr auto LOG_FILE_SDDL = L"O:SYG:SYD:P(A;;FA;;;SY)(A;;FA;;;BA)";

// 把既有檔改名成 <name>.stale-<16 hex>，讓出原本的名字；失敗回 false
static bool MoveAsideStaleLogFile(const std::wstring &path) {
  FILETIME now;
  GetSystemTimeAsFileTime(&now);
  const auto stamp = (static_cast<unsigned long long>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
  const std::wstring stale = path + std::format(L".stale-{:016x}", stamp);
  return MoveFileExW(path.c_str(), stale.c_str(), 0) != FALSE;
}

// 在 CREATE_ALWAYS 之前處理「檔案已存在」的情況（CREATE_ALWAYS 會保留既有檔的 SD，傳入的 SD 只用在新檔）：
//   - owner 是 SYSTEM 或 Administrators → 就地套用 LOG_FILE_SDDL（冪等），沿用這個檔；
//   - owner 是別人、是 reparse point／目錄／有多個 hard link、或打不開 → 改名成 .stale-<hex> 讓出名字。
// 回 true 表示可以對 path 做 CREATE_ALWAYS。
static bool PrepareExistingLogFile(const std::wstring &path, PSECURITY_DESCRIPTOR sd) {
  HANDLE file = CreateFileW(path.c_str(), READ_CONTROL | WRITE_DAC | WRITE_OWNER, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    const DWORD err = GetLastError();
    if (err == ERROR_FILE_NOT_FOUND) {
      return true;  // 新檔：CREATE_ALWAYS 會直接套用 SD
    }
    // 連 SYSTEM 都打不開（例如被人加了拒絕 ACE）：改名讓開（config 目錄的 FILE_DELETE_CHILD 仍允許）
    return MoveAsideStaleLogFile(path);
  }

  bool usable = false;
  BY_HANDLE_FILE_INFORMATION file_info {};
  if (GetFileInformationByHandle(file, &file_info) &&
      !(file_info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) &&
      file_info.nNumberOfLinks == 1) {
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR current = nullptr;
    if (GetSecurityInfo(file, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr, &current) == ERROR_SUCCESS) {
      usable = owner != nullptr && (IsWellKnownSid(owner, WinLocalSystemSid) || IsWellKnownSid(owner, WinBuiltinAdministratorsSid));
      LocalFree(current);
    }
    if (usable) {
      PSID new_owner = nullptr;
      PSID new_group = nullptr;
      PACL new_dacl = nullptr;
      BOOL present = FALSE;
      BOOL defaulted = FALSE;
      usable = GetSecurityDescriptorOwner(sd, &new_owner, &defaulted) &&
               GetSecurityDescriptorGroup(sd, &new_group, &defaulted) &&
               GetSecurityDescriptorDacl(sd, &present, &new_dacl, &defaulted) && present &&
               SetSecurityInfo(file, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, new_owner, new_group, new_dacl, nullptr) == ERROR_SUCCESS;
    }
  }
  CloseHandle(file);

  return usable || MoveAsideStaleLogFile(path);
}

HANDLE OpenLogFileHandle() {
  // VipleStream §S1-02：以前寫在 %SystemRoot%\Temp\viplestream.log。那個目錄一般使用者可以建檔，
  // 可以先建一個同名檔並保留自己的 DACL（CREATE_ALWAYS 會沿用既有檔的 SD），之後就讀得到 server 的
  // 全部 stdout。改寫到 <install>\config\viplestream-svc.log（一般使用者在 config 目錄只有 RX），
  // 並以 LOG_FILE_SDDL 建立。
  // 不建立 config 目錄（它由安裝程式／server 建立）；任何一步失敗就把 stdout 接到 NUL，
  // 絕不退回 Temp。server 自己的 sunshine.log 不受影響。

  // The file handle must be inheritable for our child process to use it
  SECURITY_ATTRIBUTES security_attributes = {sizeof(security_attributes), nullptr, TRUE};

  HANDLE handle = INVALID_HANDLE_VALUE;
  PSECURITY_DESCRIPTOR sd = nullptr;
  if (!g_install_dir.empty() && ConvertStringSecurityDescriptorToSecurityDescriptorW(LOG_FILE_SDDL, SDDL_REVISION_1, &sd, nullptr)) {
    const std::wstring config_dir = g_install_dir + L"\\config";
    const std::wstring log_file_name = config_dir + L"\\viplestream-svc.log";
    const DWORD dir_attrs = GetFileAttributesW(config_dir.c_str());
    if (dir_attrs != INVALID_FILE_ATTRIBUTES && (dir_attrs & FILE_ATTRIBUTE_DIRECTORY) && PrepareExistingLogFile(log_file_name, sd)) {
      security_attributes.lpSecurityDescriptor = sd;
      // Overwrite the old log
      handle = CreateFileW(log_file_name.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security_attributes, CREATE_ALWAYS, 0, nullptr);
      security_attributes.lpSecurityDescriptor = nullptr;
    }
    LocalFree(sd);
  }

  if (handle == INVALID_HANDLE_VALUE) {
    handle = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &security_attributes, OPEN_EXISTING, 0, nullptr);
  }
  return handle;
}

bool RunTerminationHelper(HANDLE console_token, DWORD pid) {
  WCHAR module_path[MAX_PATH];
  GetModuleFileNameW(nullptr, module_path, _countof(module_path));
  std::wstring command;

  command += L'"';
  command += module_path;
  command += L'"';
  command += std::format(L" --terminate {}", pid);

  STARTUPINFOW startup_info = {};
  startup_info.cb = sizeof(startup_info);
  startup_info.lpDesktop = (LPWSTR) L"winsta0\\default";

  // Execute ourselves as a detached process in the user session with the --terminate argument.
  // This will allow us to attach to Sunshine's console and send it a Ctrl-C event.
  PROCESS_INFORMATION process_info;
  if (!CreateProcessAsUserW(console_token, module_path, (LPWSTR) command.c_str(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT | DETACHED_PROCESS, nullptr, nullptr, &startup_info, &process_info)) {
    return false;
  }

  // Wait for the termination helper to complete
  WaitForSingleObject(process_info.hProcess, INFINITE);

  // Check the exit status of the helper process
  DWORD exit_code;
  GetExitCodeProcess(process_info.hProcess, &exit_code);

  // Cleanup handles
  CloseHandle(process_info.hProcess);
  CloseHandle(process_info.hThread);

  // If the helper process returned 0, it succeeded
  return exit_code == 0;
}

VOID WINAPI ServiceMain(DWORD dwArgc, LPTSTR *lpszArgv) {
  service_status_handle = RegisterServiceCtrlHandlerEx(SERVICE_NAME, HandlerEx, nullptr);
  if (service_status_handle == nullptr) {
    // Nothing we can really do here but terminate ourselves
    ExitProcess(GetLastError());
    return;
  }

  // Tell SCM we're starting
  service_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  service_status.dwServiceSpecificExitCode = 0;
  service_status.dwWin32ExitCode = NO_ERROR;
  service_status.dwWaitHint = 0;
  service_status.dwControlsAccepted = 0;
  service_status.dwCheckPoint = 0;
  service_status.dwCurrentState = SERVICE_START_PENDING;
  SetServiceStatus(service_status_handle, &service_status);

  // Create a manual-reset stop event
  stop_event = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  if (stop_event == nullptr) {
    // Tell SCM we failed to start
    service_status.dwWin32ExitCode = GetLastError();
    service_status.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(service_status_handle, &service_status);
    return;
  }

  // Create an auto-reset session change event
  session_change_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  if (session_change_event == nullptr) {
    // Tell SCM we failed to start
    service_status.dwWin32ExitCode = GetLastError();
    service_status.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(service_status_handle, &service_status);
    return;
  }

  auto log_file_handle = OpenLogFileHandle();
  if (log_file_handle == INVALID_HANDLE_VALUE) {
    // Tell SCM we failed to start
    service_status.dwWin32ExitCode = GetLastError();
    service_status.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(service_status_handle, &service_status);
    return;
  }

  // We can use a single STARTUPINFOEXW for all the processes that we launch
  STARTUPINFOEXW startup_info = {};
  startup_info.StartupInfo.cb = sizeof(startup_info);
  startup_info.StartupInfo.lpDesktop = (LPWSTR) L"winsta0\\default";
  startup_info.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup_info.StartupInfo.hStdInput = nullptr;
  startup_info.StartupInfo.hStdOutput = log_file_handle;
  startup_info.StartupInfo.hStdError = log_file_handle;

  // Allocate an attribute list with space for 2 entries
  startup_info.lpAttributeList = AllocateProcThreadAttributeList(2);
  if (startup_info.lpAttributeList == nullptr) {
    // Tell SCM we failed to start
    service_status.dwWin32ExitCode = GetLastError();
    service_status.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(service_status_handle, &service_status);
    return;
  }

  // Only allow Sunshine.exe to inherit the log file handle, not all inheritable handles
  UpdateProcThreadAttribute(startup_info.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &log_file_handle, sizeof(log_file_handle), nullptr, nullptr);

  // Tell SCM we're running (and stoppable now)
  service_status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_PRESHUTDOWN | SERVICE_ACCEPT_SESSIONCHANGE;
  service_status.dwCurrentState = SERVICE_RUNNING;
  SetServiceStatus(service_status_handle, &service_status);

  // Loop every 3 seconds until the stop event is set or Sunshine.exe is running
  while (WaitForSingleObject(stop_event, 3000) != WAIT_OBJECT_0) {
    auto console_session_id = WTSGetActiveConsoleSessionId();
    if (console_session_id == 0xFFFFFFFF) {
      // No console session yet
      continue;
    }

    auto console_token = DuplicateTokenForSession(console_session_id);
    if (console_token == nullptr) {
      continue;
    }

    // Job objects cannot span sessions, so we must create one for each process
    auto job_handle = CreateJobObjectForChildProcess();
    if (job_handle == nullptr) {
      CloseHandle(console_token);
      continue;
    }

    // Start viplestream-server.exe inside our job object.
    // VipleStream rebrand v1.2.93: was L"Sunshine.exe".  This is a
    // critical literal — the service helper spawns the main host
    // binary by exact filename via CreateProcessAsUserW, and Windows
    // searches the caller's CWD (the install dir).  If this string
    // doesn't match the on-disk filename the service starts but the
    // host process never spawns and HTTP 47984/47989 won't bind.
    UpdateProcThreadAttribute(startup_info.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST, &job_handle, sizeof(job_handle), nullptr, nullptr);

    PROCESS_INFORMATION process_info;
    if (!CreateProcessAsUserW(console_token, L"viplestream-server.exe", nullptr, nullptr, nullptr, TRUE, CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, (LPSTARTUPINFOW) &startup_info, &process_info)) {
      CloseHandle(console_token);
      CloseHandle(job_handle);
      continue;
    }

    bool still_running;
    do {
      // Wait for the stop event to be set, Sunshine.exe to terminate, or the console session to change
      const HANDLE wait_objects[] = {stop_event, process_info.hProcess, session_change_event};
      switch (WaitForMultipleObjects(_countof(wait_objects), wait_objects, FALSE, INFINITE)) {
        case WAIT_OBJECT_0 + 2:
          if (WTSGetActiveConsoleSessionId() == console_session_id) {
            // The active console session didn't actually change. Let Sunshine keep running.
            still_running = true;
            continue;
          }
          // Fall-through to terminate Sunshine.exe and start it again.
        case WAIT_OBJECT_0:
          // The service is shutting down, so try to gracefully terminate Sunshine.exe.
          // If it doesn't terminate in 20 seconds, we will forcefully terminate it.
          if (!RunTerminationHelper(console_token, process_info.dwProcessId) ||
              WaitForSingleObject(process_info.hProcess, 20000) != WAIT_OBJECT_0) {
            // If it won't terminate gracefully, kill it now
            TerminateProcess(process_info.hProcess, ERROR_PROCESS_ABORTED);
          }
          still_running = false;
          break;

        case WAIT_OBJECT_0 + 1:
          {
            // Sunshine terminated itself.

            DWORD exit_code;
            if (GetExitCodeProcess(process_info.hProcess, &exit_code) && exit_code == ERROR_SHUTDOWN_IN_PROGRESS) {
              // Sunshine is asking for us to shut down, so gracefully stop ourselves.
              SetEvent(stop_event);
            }
            still_running = false;
            break;
          }
      }
    } while (still_running);

    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);
    CloseHandle(console_token);
    CloseHandle(job_handle);
  }

  // Let SCM know we've stopped
  service_status.dwCurrentState = SERVICE_STOPPED;
  SetServiceStatus(service_status_handle, &service_status);
}

// This will run in a child process in the user session
int DoGracefulTermination(DWORD pid) {
  // Attach to Sunshine's console
  if (!AttachConsole(pid)) {
    return GetLastError();
  }

  // Disable our own Ctrl-C handling
  SetConsoleCtrlHandler(nullptr, TRUE);

  // Send a Ctrl-C event to Sunshine
  if (!GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0)) {
    return GetLastError();
  }

  return 0;
}

int main(int argc, char *argv[]) {
  static const SERVICE_TABLE_ENTRY service_table[] = {
    {(LPSTR) SERVICE_NAME, ServiceMain},
    {nullptr, nullptr}
  };

  // Check if this is a reinvocation of ourselves to send Ctrl-C to Sunshine.exe
  if (argc == 3 && strcmp(argv[1], "--terminate") == 0) {
    return DoGracefulTermination(atol(argv[2]));
  }

  // By default, services have their current directory set to %SYSTEMROOT%\System32.
  // We want to use the directory where Sunshine.exe is located instead of system32.
  // This requires stripping off 2 path components: the file name and the last folder
  WCHAR module_path[MAX_PATH];
  GetModuleFileNameW(nullptr, module_path, _countof(module_path));
  for (auto i = 0; i < 2; i++) {
    auto last_sep = wcsrchr(module_path, '\\');
    if (last_sep) {
      *last_sep = 0;
    }
  }
  SetCurrentDirectoryW(module_path);
  g_install_dir = module_path;  // VipleStream §S1-02：viplestream-svc.log 放在 <install>\config

  // Trigger our ServiceMain()
  return StartServiceCtrlDispatcher(service_table);
}

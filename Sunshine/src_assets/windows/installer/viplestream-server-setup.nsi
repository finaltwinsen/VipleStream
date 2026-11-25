; §SERVER-SETUP：VipleStream Server 的 Windows 安裝程式（NSIS）。
;
; 為什麼要有：只出 zip 時，使用者常直接從「下載」資料夾執行、沒註冊成服務（VR 需要 SYSTEM 服務），
; 也沒裝 ViGEmBus。這支安裝程式把 README 的手動步驟做完。zip 照樣出：server 的自動更新靠固定的 zip 檔名。
;
; 提權與服務的安全原則（改這支腳本前先讀）：
;   1. 安裝目錄固定在 Program Files，不給選。服務以 LocalSystem 執行，執行檔若放在一般使用者寫得進去的
;      目錄，任何使用者換掉 exe 就能拿到 SYSTEM。Program Files 只有系統管理員能寫。
;   2. 服務的 ImagePath 一律加引號（"C:\Program Files\...\viplestream-svc.exe"）。沒加引號時
;      Windows 會依序嘗試 C:\Program.exe 等路徑。舊版手動註冊的服務沒加引號的，這裡一併改正。
;   3. 所有外部指令用 $SYSDIR 下的完整路徑，不靠 PATH；PowerShell 指令全部是固定字串，只插入固定的
;      $INSTDIR（Program Files 底下、不含單引號）。不從使用者可寫的位置執行任何腳本。
;   4. 先停服務、確認停下，才覆蓋檔案；停不下來就中止，不動任何檔案。
;   5. 不覆蓋 config\（設定、配對憑證、帳密）。解除安裝預設也保留它。
;   6. 不動其他廠商的服務：偵測到 Sunshine／Apollo 服務只詢問，使用者同意才停用（不刪除）。
;
; 建置：build_sunshine.cmd 在打完 zip 之後呼叫
;   makensis /DVERSION=X.Y.Z /DSTAGE=<zip 的暫存目錄> /DOUTFILE=<輸出 exe> /DICON=<ico> 本檔

Unicode true
ManifestDPIAware true
SetCompressor /SOLID lzma

!ifndef VERSION
  !error "VERSION is required"
!endif
!ifndef STAGE
  !error "STAGE is required"
!endif
!ifndef OUTFILE
  !error "OUTFILE is required"
!endif

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "x64.nsh"
!include "FileFunc.nsh"

!define PRODUCT "VipleStream Server"
!define SVC "VipleStreamServer"
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\VipleStreamServer"
!define FW_RULE "VipleStream-Server"
!define WEBUI "https://localhost:47990"

Name "${PRODUCT}"
OutFile "${OUTFILE}"
InstallDir "$PROGRAMFILES64\VipleStream-Server"
RequestExecutionLevel admin
ShowInstDetails show
ShowUninstDetails show
BrandingText "${PRODUCT} ${VERSION}"

VIProductVersion "${VERSION}.0"
VIAddVersionKey /LANG=1033 "ProductName" "${PRODUCT}"
VIAddVersionKey /LANG=1033 "ProductVersion" "${VERSION}"
VIAddVersionKey /LANG=1033 "FileVersion" "${VERSION}"
VIAddVersionKey /LANG=1033 "FileDescription" "${PRODUCT} Setup"
VIAddVersionKey /LANG=1033 "LegalCopyright" "GPL-3.0"

!ifdef ICON
  !define MUI_ICON "${ICON}"
  !define MUI_UNICON "${ICON}"
!endif
!define MUI_ABORTWARNING

!define MUI_FINISHPAGE_SHOWREADME ""
!define MUI_FINISHPAGE_SHOWREADME_TEXT "$(TXT_OPEN_WEBUI)"
!define MUI_FINISHPAGE_SHOWREADME_FUNCTION OpenWebUi

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"
!insertmacro MUI_LANGUAGE "TradChinese"

LangString TXT_OPEN_WEBUI ${LANG_ENGLISH} "Open the web settings page (create your login there)"
LangString TXT_OPEN_WEBUI ${LANG_TRADCHINESE} "開啟網頁設定頁（在那裡建立登入帳密）"
LangString TXT_NEED_X64 ${LANG_ENGLISH} "${PRODUCT} needs 64-bit Windows 10 or 11."
LangString TXT_NEED_X64 ${LANG_TRADCHINESE} "${PRODUCT} 需要 64 位元的 Windows 10 或 11。"
LangString TXT_NEED_ADMIN ${LANG_ENGLISH} "Setup must be run by an administrator."
LangString TXT_NEED_ADMIN ${LANG_TRADCHINESE} "請用系統管理員帳號執行安裝程式。"
LangString TXT_ALREADY ${LANG_ENGLISH} "Setup is already running."
LangString TXT_ALREADY ${LANG_TRADCHINESE} "安裝程式已經在執行中。"
LangString TXT_STOP_FAIL ${LANG_ENGLISH} "The ${PRODUCT} service did not stop. Nothing was changed. Close any stream, then run Setup again."
LangString TXT_STOP_FAIL ${LANG_TRADCHINESE} "${PRODUCT} 服務停不下來，沒有變更任何檔案。請先結束串流，再重新執行安裝程式。"
LangString TXT_FOREIGN ${LANG_ENGLISH} "Another copy of ${PRODUCT} is running from a different folder:$\r$\n$0$\r$\n$\r$\nIt uses the same network ports. End it now?"
LangString TXT_FOREIGN ${LANG_TRADCHINESE} "另一份 ${PRODUCT} 正從別的資料夾執行：$\r$\n$0$\r$\n$\r$\n它會佔用相同的連接埠。要現在結束它嗎？"
LangString TXT_OTHER_SVC ${LANG_ENGLISH} "The $0 service is installed and uses the same network ports as ${PRODUCT}.$\r$\n$\r$\nStop it and set it to Disabled? (It is not removed; you can turn it back on in Services.)"
LangString TXT_OTHER_SVC ${LANG_TRADCHINESE} "已安裝 $0 服務，它會佔用跟 ${PRODUCT} 相同的連接埠。$\r$\n$\r$\n要停止它並設成「停用」嗎？（不會移除，之後可以在「服務」裡重新啟用。）"
LangString TXT_START_FAIL ${LANG_ENGLISH} "Files were installed, but the ${PRODUCT} service did not start. Open Services, start ${SVC}, and check config\sunshine.log."
LangString TXT_START_FAIL ${LANG_TRADCHINESE} "檔案已安裝，但 ${PRODUCT} 服務沒有啟動。請在「服務」裡啟動 ${SVC}，並查看 config\sunshine.log。"
LangString TXT_UN_CONFIG ${LANG_ENGLISH} "Also delete settings, paired devices and the web login (the config folder)?$\r$\n$\r$\nChoose No to keep them for a later reinstall."
LangString TXT_UN_CONFIG ${LANG_TRADCHINESE} "要一併刪除設定、已配對裝置與網頁帳密（config 資料夾）嗎？$\r$\n$\r$\n選「否」會保留，之後重新安裝可以沿用。"
LangString TXT_UN_BADDIR ${LANG_ENGLISH} "Unexpected install folder; files were not removed: $INSTDIR"
LangString TXT_UN_BADDIR ${LANG_TRADCHINESE} "安裝資料夾不符預期，沒有刪除檔案：$INSTDIR"
LangString TXT_LOG_CONSENT ${LANG_ENGLISH} "Help improve VipleStream?$\r$\n$\r$\nWhen a stream ends or the server stops unexpectedly, send an anonymous log to the developer. It contains the server log, version and Windows version. IP addresses, computer names and user names are removed first. No account or personal data is included. Logs are stored in the developer's Google Drive.$\r$\n$\r$\nYou can change this later on the web settings page."
LangString TXT_LOG_CONSENT ${LANG_TRADCHINESE} "協助改善 VipleStream？$\r$\n$\r$\n在串流結束與 server 異常結束時，自動傳送匿名 log 給開發者。內容是 server log、版本與 Windows 版本；IP、電腦名稱、使用者名稱會先遮掉，不含帳號或個人資料；存在開發者的 Google 雲端硬碟。$\r$\n$\r$\n之後可以在網頁設定頁更改。"
LangString DESC_CORE ${LANG_ENGLISH} "${PRODUCT} (required)"
LangString DESC_CORE ${LANG_TRADCHINESE} "${PRODUCT}（必要）"
LangString DESC_VIGEM ${LANG_ENGLISH} "ViGEmBus gamepad driver"
LangString DESC_VIGEM ${LANG_TRADCHINESE} "ViGEmBus 手把驅動程式"

; ---------------------------------------------------------------------------
; 共用：以固定字串呼叫系統工具。外層用反引號：PowerShell 指令裡用的是單引號。$SYSDIR 在 32 位元的 NSIS 下會被轉到 SysWOW64，
; 所以 PowerShell 與 netsh 都先關掉轉向，用 64 位元的版本（服務、防火牆、QoS 的 cmdlet 才完整）。
;
; 工具的輸出一律不放進安裝紀錄，只印自己寫的英文狀態行：中文 Windows 上 sc／netsh 用 OEM 字碼頁輸出，
; PowerShell 第一次載入模組時還會把進度訊息以 CLIXML 印到 stderr，兩者都會變亂碼。
; $ProgressPreference 關掉進度訊息，PS_STACK 讀回來比對的字串才不會混進 CLIXML。
!define PS_EXE `"$SYSDIR\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command`
!macro PS CMD
  ${DisableX64FSRedirection}
  nsExec::ExecToStack `${PS_EXE} "$$ProgressPreference='SilentlyContinue'; ${CMD}"`
  Pop $9
  Pop $8
  ${EnableX64FSRedirection}
  ${If} $9 != 0
    DetailPrint "  (PowerShell step exit code $9)"
  ${EndIf}
!macroend
!macro PS_STACK CMD
  ${DisableX64FSRedirection}
  nsExec::ExecToStack `${PS_EXE} "$$ProgressPreference='SilentlyContinue'; ${CMD}"`
  Pop $9
  Pop $0
  ${EnableX64FSRedirection}
!macroend
!macro SC ARGS
  ${DisableX64FSRedirection}
  nsExec::ExecToStack '"$SYSDIR\sc.exe" ${ARGS}'
  Pop $9
  Pop $8
  ${EnableX64FSRedirection}
!macroend
!macro NETSH ARGS
  ${DisableX64FSRedirection}
  nsExec::ExecToStack '"$SYSDIR\netsh.exe" ${ARGS}'
  Pop $9
  Pop $8
  ${EnableX64FSRedirection}
!macroend

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "$(TXT_NEED_X64)" /SD IDOK
    Abort
  ${EndIf}
  SetRegView 64
  UserInfo::GetAccountType
  Pop $0
  ${If} $0 != "admin"
    MessageBox MB_ICONSTOP "$(TXT_NEED_ADMIN)" /SD IDOK
    Abort
  ${EndIf}
  System::Call 'kernel32::CreateMutexW(p 0, i 1, w "VipleStreamServerSetup") p .r1 ?e'
  Pop $0
  ${If} $0 = 183
    MessageBox MB_ICONEXCLAMATION "$(TXT_ALREADY)" /SD IDOK
    Abort
  ${EndIf}
  ; 介面語言跟著 Windows 顯示語言：繁中系統用繁中，其他用英文
  System::Call 'kernel32::GetUserDefaultUILanguage() i .r0'
  ${If} $0 = 1028
  ${OrIf} $0 = 3076
  ${OrIf} $0 = 5124
    StrCpy $LANGUAGE ${LANG_TRADCHINESE}
  ${Else}
    StrCpy $LANGUAGE ${LANG_ENGLISH}
  ${EndIf}
FunctionEnd

Function un.onInit
  SetRegView 64
  UserInfo::GetAccountType
  Pop $0
  ${If} $0 != "admin"
    MessageBox MB_ICONSTOP "$(TXT_NEED_ADMIN)" /SD IDOK
    Abort
  ${EndIf}
FunctionEnd

; 以 explorer 開網址：安裝程式是提權執行的，直接 ExecShell 會讓瀏覽器也跑在系統管理員身分。
Function OpenWebUi
  ${DisableX64FSRedirection}
  Exec '"$WINDIR\explorer.exe" "${WEBUI}"'
  ${EnableX64FSRedirection}
FunctionEnd

; 停止本服務並確認停下；停不下就中止安裝（還沒動任何檔案）。
Function StopOwnService
  DetailPrint "Stopping ${SVC} if it is running..."
  !insertmacro PS "$$s = Get-Service -Name '${SVC}' -ErrorAction SilentlyContinue; if ($$s -and $$s.Status -ne 'Stopped') { Stop-Service -Name '${SVC}' -Force -ErrorAction SilentlyContinue; $$t = 0; while ((Get-Service -Name '${SVC}').Status -ne 'Stopped' -and $$t -lt 30) { Start-Sleep -Seconds 1; $$t++ } }; if ($$s -and (Get-Service -Name '${SVC}').Status -ne 'Stopped') { Get-Process -Name 'viplestream-svc','viplestream-server' -ErrorAction SilentlyContinue | Where-Object { $$_.Path -like '$INSTDIR\*' } | Stop-Process -Force -ErrorAction SilentlyContinue; Start-Sleep -Seconds 3 }"
  !insertmacro PS_STACK "$$s = Get-Service -Name '${SVC}' -ErrorAction SilentlyContinue; if ($$s -and $$s.Status -ne 'Stopped') { 'running' } else { 'ok' }"
  ${If} $0 != "ok$\r$\n"
  ${AndIf} $0 != "ok"
    MessageBox MB_ICONSTOP "$(TXT_STOP_FAIL)" /SD IDOK
    Abort
  ${EndIf}
  ; 安裝目錄裡以主控台模式跑的 server（不是服務）也要收掉，否則 exe 被鎖住
  !insertmacro PS "Get-Process -Name 'viplestream-server' -ErrorAction SilentlyContinue | Where-Object { $$_.Path -like '$INSTDIR\*' } | Stop-Process -Force -ErrorAction SilentlyContinue"
FunctionEnd

; 從別的資料夾執行的 server（例如直接在「下載」裡開的）會搶連接埠。先問，同意才結束。
Function HandleForeignServer
  !insertmacro PS_STACK "(Get-Process -Name 'viplestream-server' -ErrorAction SilentlyContinue | Where-Object { $$_.Path -and $$_.Path -notlike '$INSTDIR\*' } | Select-Object -ExpandProperty Path -Unique) -join ', '"
  ${If} $0 == ""
  ${OrIf} $0 == "$\r$\n"
    Return
  ${EndIf}
  MessageBox MB_YESNO|MB_ICONQUESTION "$(TXT_FOREIGN)" /SD IDYES IDNO done
  !insertmacro PS "Get-Process -Name 'viplestream-server' -ErrorAction SilentlyContinue | Where-Object { $$_.Path -and $$_.Path -notlike '$INSTDIR\*' } | Stop-Process -Force -ErrorAction SilentlyContinue"
  done:
FunctionEnd

; 上游 Sunshine／Apollo 的服務。只詢問；同意才停止並設成停用，不刪除。
!macro OTHER_SERVICE NAME
  !insertmacro PS_STACK "if (Get-Service -Name '${NAME}' -ErrorAction SilentlyContinue) { 'yes' } else { 'no' }"
  ${If} $0 == "yes$\r$\n"
  ${OrIf} $0 == "yes"
    StrCpy $0 "${NAME}"
    StrCpy $8 "no"
    MessageBox MB_YESNO|MB_ICONQUESTION "$(TXT_OTHER_SVC)" /SD IDNO IDNO +2
    StrCpy $8 "yes"
    ${If} $8 == "yes"
      !insertmacro PS "Stop-Service -Name '${NAME}' -Force -ErrorAction SilentlyContinue; Set-Service -Name '${NAME}' -StartupType Disabled"
      DetailPrint "Disabled ${NAME}"
    ${EndIf}
  ${EndIf}
!macroend

Section "!$(DESC_CORE)" SecCore
  SectionIn RO
  SetShellVarContext all
  SetRegView 64

  Call HandleForeignServer
  !insertmacro OTHER_SERVICE "SunshineService"
  !insertmacro OTHER_SERVICE "ApolloService"
  Call StopOwnService

  ; config\ 不在暫存目錄裡，既有的設定、配對與帳密不會被覆蓋
  SetOutPath "$INSTDIR"
  SetOverwrite on
  File /r "${STAGE}\*.*"

  ; 服務程式要在下一層（viplestream-svc 以自己的上一層找 viplestream-server.exe）
  CreateDirectory "$INSTDIR\tools"
  CopyFiles /SILENT "$INSTDIR\viplestream-svc.exe" "$INSTDIR\tools\viplestream-svc.exe"

  ; 服務：不存在就建立；存在就保留帳戶等設定，只改正路徑與啟動類型。
  !insertmacro PS_STACK "if (Get-Service -Name '${SVC}' -ErrorAction SilentlyContinue) { 'yes' } else { 'no' }"
  ${If} $0 == "no$\r$\n"
  ${OrIf} $0 == "no"
    DetailPrint "Creating service ${SVC}"
    !insertmacro SC 'create ${SVC} binPath= "\"$INSTDIR\tools\viplestream-svc.exe\"" start= auto DisplayName= "${PRODUCT}"'
  ${EndIf}
  ; 直接寫登錄的 ImagePath，確保一定帶引號（也改正舊版沒加引號、或指向「下載」資料夾的服務）
  WriteRegExpandStr HKLM "SYSTEM\CurrentControlSet\Services\${SVC}" "ImagePath" '"$INSTDIR\tools\viplestream-svc.exe"'
  DetailPrint "Configuring service ${SVC} (automatic start, quoted path)"
  !insertmacro SC 'config ${SVC} start= auto'
  !insertmacro SC 'description ${SVC} "Game and VR streaming server for VipleStream clients."'
  !insertmacro SC 'failure ${SVC} reset= 86400 actions= restart/10000/restart/10000//0'

  ; 防火牆：只放行這支程式（不是開整個連接埠範圍）。先刪同名與舊版手動說明建立的規則，避免重複。
  DetailPrint "Configuring Windows Firewall..."
  !insertmacro NETSH 'advfirewall firewall delete rule name="${FW_RULE}"'
  !insertmacro NETSH 'advfirewall firewall delete rule name="VipleStream Server"'
  !insertmacro NETSH 'advfirewall firewall add rule name="${FW_RULE}" dir=in action=allow protocol=tcp program="$INSTDIR\viplestream-server.exe" enable=yes'
  !insertmacro NETSH 'advfirewall firewall add rule name="${FW_RULE}" dir=in action=allow protocol=udp program="$INSTDIR\viplestream-server.exe" enable=yes'
  ${If} $9 != 0
    DetailPrint "  (firewall rule exit code $9)"
  ${EndIf}
  ; Q-DSCP：同 add-firewall-rule.bat，MP-QUIC（UDP 來源埠 48010）標成 DSCP 40，Wi-Fi 才會進影像佇列
  !insertmacro PS "Remove-NetQosPolicy -Name 'VipleStream-Server-QUIC' -Confirm:$$false -ErrorAction SilentlyContinue; New-NetQosPolicy -Name 'VipleStream-Server-QUIC' -AppPathNameMatchCondition 'viplestream-server.exe' -IPProtocolMatchCondition UDP -IPSrcPortStartMatchCondition 48010 -IPSrcPortEndMatchCondition 48010 -DSCPAction 40 -NetworkProfile All | Out-Null"

  ; 開始功能表
  CreateDirectory "$SMPROGRAMS\${PRODUCT}"
  WriteINIStr "$SMPROGRAMS\${PRODUCT}\${PRODUCT} Web UI.url" "InternetShortcut" "URL" "${WEBUI}"
  CreateShortCut "$SMPROGRAMS\${PRODUCT}\Uninstall ${PRODUCT}.lnk" "$INSTDIR\uninstall.exe"

  ; 解除安裝資訊
  WriteUninstaller "$INSTDIR\uninstall.exe"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayName" "${PRODUCT}"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNINST_KEY}" "Publisher" "VipleStream"
  WriteRegStr HKLM "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\viplestream-server.exe"
  WriteRegStr HKLM "${UNINST_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegStr HKLM "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoRepair" 1
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD HKLM "${UNINST_KEY}" "EstimatedSize" "$0"
SectionEnd

Section "$(DESC_VIGEM)" SecVigem
  SetRegView 64
  ; 已裝就不重裝（安裝程式會要求重新開機的情況也一併避開）
  !insertmacro PS_STACK "if (Get-Service -Name 'ViGEmBus' -ErrorAction SilentlyContinue) { 'yes' } else { 'no' }"
  ${If} $0 == "yes$\r$\n"
  ${OrIf} $0 == "yes"
    DetailPrint "ViGEmBus is already installed"
  ${ElseIf} ${FileExists} "$INSTDIR\scripts\vigembus_installer.exe"
    DetailPrint "Installing ViGEmBus..."
    ExecWait '"$INSTDIR\scripts\vigembus_installer.exe" /quiet /norestart' $1
    DetailPrint "ViGEmBus installer exit code: $1"
  ${EndIf}
SectionEnd

; §LOGDROP：安裝時就問一次匿名 log（跟網頁首頁的詢問寫同一個設定）。設定檔裡已經有 log_upload 就不再問，
; 重裝、升級都不會覆蓋使用者之前的選擇。靜默安裝（/S）不寫，交給首頁詢問。
Section "-LogConsent"
  SetRegView 64
  IfSilent consent_done
  !insertmacro PS_STACK "if (Select-String -LiteralPath '$INSTDIR\config\sunshine.conf' -Pattern '^\s*log_upload\s*=' -Quiet -ErrorAction SilentlyContinue) { 'set' } else { 'ask' }"
  ${If} $0 == "set$\r$\n"
  ${OrIf} $0 == "set"
    Goto consent_done
  ${EndIf}
  StrCpy $7 "disabled"
  MessageBox MB_YESNO|MB_ICONQUESTION "$(TXT_LOG_CONSENT)" IDNO +2
  StrCpy $7 "enabled"
  CreateDirectory "$INSTDIR\config"
  FileOpen $6 "$INSTDIR\config\sunshine.conf" a
  FileSeek $6 0 END
  FileWrite $6 "$\r$\nlog_upload = $7$\r$\n"
  FileClose $6
  DetailPrint "Anonymous log upload: $7"
  consent_done:
SectionEnd

; 服務最後才啟動：ViGEmBus 若剛裝好，server 啟動時就偵測得到
Section "-StartService"
  SetRegView 64
  DetailPrint "Starting ${SVC}..."
  !insertmacro PS_STACK "Start-Service -Name '${SVC}' -ErrorAction SilentlyContinue; $$t = 0; while ((Get-Service -Name '${SVC}').Status -ne 'Running' -and $$t -lt 30) { Start-Sleep -Seconds 1; $$t++ }; (Get-Service -Name '${SVC}').Status"
  DetailPrint "Service status: $0"
  ${If} $0 != "Running$\r$\n"
  ${AndIf} $0 != "Running"
    MessageBox MB_ICONEXCLAMATION "$(TXT_START_FAIL)" /SD IDOK
  ${EndIf}
SectionEnd

Section "Uninstall"
  SetShellVarContext all
  SetRegView 64

  ; 保險：只在預期的資料夾名稱下刪檔，避免 $INSTDIR 異常時誤刪
  ${GetFileName} "$INSTDIR" $0
  ${If} $0 != "VipleStream-Server"
    MessageBox MB_ICONSTOP "$(TXT_UN_BADDIR)" /SD IDOK
    Abort
  ${EndIf}

  !insertmacro PS "Stop-Service -Name '${SVC}' -Force -ErrorAction SilentlyContinue; $$t = 0; while ((Get-Service -Name '${SVC}' -ErrorAction SilentlyContinue).Status -eq 'Running' -and $$t -lt 30) { Start-Sleep -Seconds 1; $$t++ }; Get-Process -Name 'viplestream-svc','viplestream-server' -ErrorAction SilentlyContinue | Where-Object { $$_.Path -like '$INSTDIR\*' } | Stop-Process -Force -ErrorAction SilentlyContinue"
  DetailPrint "Removing service ${SVC}"
  !insertmacro SC 'delete ${SVC}'

  !insertmacro NETSH 'advfirewall firewall delete rule name="${FW_RULE}"'
  !insertmacro PS "Remove-NetQosPolicy -Name 'VipleStream-Server-QUIC' -Confirm:$$false -ErrorAction SilentlyContinue"

  ; 刪掉 config 以外的全部內容
  !insertmacro PS "Get-ChildItem -LiteralPath '$INSTDIR' -Force | Where-Object { $$_.Name -ne 'config' -and $$_.Name -ne 'uninstall.exe' } | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue"
  MessageBox MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2 "$(TXT_UN_CONFIG)" /SD IDNO IDNO keepconfig
  RMDir /r "$INSTDIR\config"
  keepconfig:
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"

  Delete "$SMPROGRAMS\${PRODUCT}\${PRODUCT} Web UI.url"
  Delete "$SMPROGRAMS\${PRODUCT}\Uninstall ${PRODUCT}.lnk"
  RMDir "$SMPROGRAMS\${PRODUCT}"
  DeleteRegKey HKLM "${UNINST_KEY}"
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecCore} "$(DESC_CORE)"
  !insertmacro MUI_DESCRIPTION_TEXT ${SecVigem} "$(DESC_VIGEM)"
!insertmacro MUI_FUNCTION_DESCRIPTION_END

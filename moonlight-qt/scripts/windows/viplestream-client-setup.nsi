; §CLIENT-SETUP：VipleStream Windows client 的安裝程式（NSIS）。
;
; zip 照出：client 的自動更新靠固定的 zip 檔名（app/backend/updateassetrules.h），
; 裝在 Program Files 時 updater 會自己要系統管理員權限覆蓋（updater.cpp 的 shellExecuteRunas）。
; 這支安裝程式只負責第一次安裝：放檔案、開始功能表與桌面捷徑、「新增或移除程式」登記、解除安裝。
;
; 原則：
;   - 外部指令只用 $SYSDIR 下的完整路徑、固定字串；不從使用者可寫的位置執行腳本。
;   - 安裝前若 VipleStream.exe 正在執行，先問，同意才結束它（串流中直接覆蓋 DLL 會失敗或損壞）。
;   - 使用者設定（登錄檔 HKCU\Software\VipleStream、主機配對）安裝與升級都不動；解除安裝預設保留。
;   - 結束頁「啟動 VipleStream」用 explorer 開，不讓 client 跟著安裝程式以系統管理員身分執行。
;
; 建置：build-tools\build_moonlight_package.cmd 在打完 zip 之後呼叫
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

!define PRODUCT "VipleStream"
!define EXE "VipleStream.exe"
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\VipleStreamClient"

Name "${PRODUCT}"
OutFile "${OUTFILE}"
InstallDir "$PROGRAMFILES64\VipleStream"
InstallDirRegKey HKLM "${UNINST_KEY}" "InstallLocation"
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

!define MUI_FINISHPAGE_RUN ""
!define MUI_FINISHPAGE_RUN_TEXT "$(TXT_LAUNCH)"
!define MUI_FINISHPAGE_RUN_FUNCTION LaunchClient

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"
!insertmacro MUI_LANGUAGE "TradChinese"

LangString TXT_LAUNCH ${LANG_ENGLISH} "Start VipleStream"
LangString TXT_LAUNCH ${LANG_TRADCHINESE} "啟動 VipleStream"
LangString TXT_NEED_X64 ${LANG_ENGLISH} "${PRODUCT} needs 64-bit Windows 10 or 11."
LangString TXT_NEED_X64 ${LANG_TRADCHINESE} "${PRODUCT} 需要 64 位元的 Windows 10 或 11。"
LangString TXT_ALREADY ${LANG_ENGLISH} "Setup is already running."
LangString TXT_ALREADY ${LANG_TRADCHINESE} "安裝程式已經在執行中。"
LangString TXT_RUNNING ${LANG_ENGLISH} "VipleStream is running. Close it now and continue?$\r$\n$\r$\nAny stream in progress will end."
LangString TXT_RUNNING ${LANG_TRADCHINESE} "VipleStream 正在執行。要現在關閉它並繼續安裝嗎？$\r$\n$\r$\n進行中的串流會結束。"
LangString TXT_STILL_RUNNING ${LANG_ENGLISH} "VipleStream is still running. Close it, then run Setup again. Nothing was changed."
LangString TXT_STILL_RUNNING ${LANG_TRADCHINESE} "VipleStream 仍在執行。請先關閉它，再重新執行安裝程式。沒有變更任何檔案。"
LangString TXT_UN_SETTINGS ${LANG_ENGLISH} "Also delete settings and paired computers?$\r$\n$\r$\nChoose No to keep them for a later reinstall."
LangString TXT_UN_SETTINGS ${LANG_TRADCHINESE} "要一併刪除設定與已配對的電腦嗎？$\r$\n$\r$\n選「否」會保留，之後重新安裝可以沿用。"
LangString TXT_UN_BADDIR ${LANG_ENGLISH} "Unexpected install folder; files were not removed: $INSTDIR"
LangString TXT_UN_BADDIR ${LANG_TRADCHINESE} "安裝資料夾不符預期，沒有刪除檔案：$INSTDIR"
LangString DESC_CORE ${LANG_ENGLISH} "${PRODUCT} (required)"
LangString DESC_CORE ${LANG_TRADCHINESE} "${PRODUCT}（必要）"
LangString DESC_DESKTOP ${LANG_ENGLISH} "Desktop shortcut"
LangString DESC_DESKTOP ${LANG_TRADCHINESE} "桌面捷徑"

; 外層用反引號：PowerShell 指令裡用的是單引號。輸出不放進安裝紀錄（中文 Windows 的 OEM 字碼頁會變亂碼）。
!define PS_EXE `"$SYSDIR\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command`
!macro PS_STACK CMD
  ${DisableX64FSRedirection}
  nsExec::ExecToStack `${PS_EXE} "$$ProgressPreference='SilentlyContinue'; ${CMD}"`
  Pop $9
  Pop $0
  ${EnableX64FSRedirection}
!macroend

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "$(TXT_NEED_X64)" /SD IDOK
    Abort
  ${EndIf}
  SetRegView 64
  System::Call 'kernel32::CreateMutexW(p 0, i 1, w "VipleStreamClientSetup") p .r1 ?e'
  Pop $0
  ${If} $0 = 183
    MessageBox MB_ICONEXCLAMATION "$(TXT_ALREADY)" /SD IDOK
    Abort
  ${EndIf}
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
FunctionEnd

Function LaunchClient
  ${DisableX64FSRedirection}
  Exec '"$WINDIR\explorer.exe" "$INSTDIR\${EXE}"'
  ${EnableX64FSRedirection}
FunctionEnd

; 只處理「從安裝目錄執行」的那份。使用者另外解壓在別處的 zip 版不碰。
Function CloseRunningClient
  !insertmacro PS_STACK "if (Get-Process -Name 'VipleStream' -ErrorAction SilentlyContinue | Where-Object { $$_.Path -like '$INSTDIR\*' }) { 'yes' } else { 'no' }"
  ${If} $0 != "yes$\r$\n"
  ${AndIf} $0 != "yes"
    Return
  ${EndIf}
  MessageBox MB_YESNO|MB_ICONQUESTION "$(TXT_RUNNING)" /SD IDYES IDYES close
  Abort
  close:
  !insertmacro PS_STACK "Get-Process -Name 'VipleStream' -ErrorAction SilentlyContinue | Where-Object { $$_.Path -like '$INSTDIR\*' } | ForEach-Object { $$_.CloseMainWindow() | Out-Null }; Start-Sleep -Seconds 3; Get-Process -Name 'VipleStream' -ErrorAction SilentlyContinue | Where-Object { $$_.Path -like '$INSTDIR\*' } | Stop-Process -Force -ErrorAction SilentlyContinue; Start-Sleep -Seconds 1; if (Get-Process -Name 'VipleStream' -ErrorAction SilentlyContinue | Where-Object { $$_.Path -like '$INSTDIR\*' }) { 'running' } else { 'ok' }"
  ${If} $0 != "ok$\r$\n"
  ${AndIf} $0 != "ok"
    MessageBox MB_ICONSTOP "$(TXT_STILL_RUNNING)" /SD IDOK
    Abort
  ${EndIf}
FunctionEnd

Section "!$(DESC_CORE)" SecCore
  SectionIn RO
  SetShellVarContext all
  SetRegView 64
  Call CloseRunningClient

  SetOutPath "$INSTDIR"
  SetOverwrite on
  File /r "${STAGE}\*.*"

  CreateDirectory "$SMPROGRAMS\${PRODUCT}"
  CreateShortCut "$SMPROGRAMS\${PRODUCT}\${PRODUCT}.lnk" "$INSTDIR\${EXE}"
  CreateShortCut "$SMPROGRAMS\${PRODUCT}\Uninstall ${PRODUCT}.lnk" "$INSTDIR\uninstall.exe"

  WriteUninstaller "$INSTDIR\uninstall.exe"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayName" "${PRODUCT}"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNINST_KEY}" "Publisher" "VipleStream"
  WriteRegStr HKLM "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\${EXE}"
  WriteRegStr HKLM "${UNINST_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegStr HKLM "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoRepair" 1
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD HKLM "${UNINST_KEY}" "EstimatedSize" "$0"
SectionEnd

Section "$(DESC_DESKTOP)" SecDesktop
  SetShellVarContext all
  CreateShortCut "$DESKTOP\${PRODUCT}.lnk" "$INSTDIR\${EXE}"
SectionEnd

Section "Uninstall"
  SetShellVarContext all
  SetRegView 64
  ; 保險：安裝位置可以自選，所以不看資料夾名稱，改成「裡面真的有 client 主程式、而且不是磁碟根目錄」才刪
  StrLen $1 "$INSTDIR"
  ${IfNot} ${FileExists} "$INSTDIR\${EXE}"
  ${OrIf} $1 <= 3
    MessageBox MB_ICONSTOP "$(TXT_UN_BADDIR)" /SD IDOK
    Abort
  ${EndIf}
  !insertmacro PS_STACK "Get-Process -Name 'VipleStream' -ErrorAction SilentlyContinue | Where-Object { $$_.Path -like '$INSTDIR\*' } | Stop-Process -Force -ErrorAction SilentlyContinue"
  !insertmacro PS_STACK "Get-ChildItem -LiteralPath '$INSTDIR' -Force | Where-Object { $$_.Name -ne 'uninstall.exe' } | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue"
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"

  Delete "$DESKTOP\${PRODUCT}.lnk"
  Delete "$SMPROGRAMS\${PRODUCT}\${PRODUCT}.lnk"
  Delete "$SMPROGRAMS\${PRODUCT}\Uninstall ${PRODUCT}.lnk"
  RMDir "$SMPROGRAMS\${PRODUCT}"
  DeleteRegKey HKLM "${UNINST_KEY}"

  ; 設定與配對在執行解除安裝的這個使用者的 HKCU 與 AppData；預設保留
  MessageBox MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2 "$(TXT_UN_SETTINGS)" /SD IDNO IDNO keepsettings
  SetShellVarContext current
  DeleteRegKey HKCU "Software\VipleStream\VipleStream"
  RMDir /r "$LOCALAPPDATA\VipleStream"
  RMDir /r "$APPDATA\VipleStream"
  keepsettings:
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecCore} "$(DESC_CORE)"
  !insertmacro MUI_DESCRIPTION_TEXT ${SecDesktop} "$(DESC_DESKTOP)"
!insertmacro MUI_FUNCTION_DESCRIPTION_END

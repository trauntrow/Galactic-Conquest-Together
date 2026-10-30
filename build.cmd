@echo off
rem Builds dinput8.dll and runs every test.
rem "build.cmd" makes the debug build in out\Debug; "build.cmd release" makes out\Release and the zip.
setlocal
cd /d "%~dp0"

set "CONFIG=Debug"
set "DLL_FLAGS=/Zi /O2 /DGC_DEV=1"
set "DLL_LINK="
set "DLL_RES="
set "DLL_LIBS=ws2_32.lib"
if /i "%~1"=="release" (
  set "CONFIG=Release"
  set "DLL_FLAGS=/O2 /Brepro /DGC_DEV=0"
  set "DLL_LINK=/OPT:REF /OPT:ICF /Brepro"
  set "DLL_RES=player.res"
  set "DLL_LIBS="
) else if not "%~1"=="" (
  echo usage: build.cmd [release]
  exit /b 2
)
set "PLAYER_FLAGS=/DGC_PLAYER=1 /DGC_PLAYER_MENU=1 /DGC_LUA_QUERY=1 /DGC_SCREEN_OBSERVER=1 /DGC_CAMPAIGN_CAPTURE=1 /DGC_SESSION=1 /DGC_PLAYER_APPLY=1"
set "CXX=cl /nologo /std:c++17 /EHsc /W4 /MT"

set "VSDIR="
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
  echo Visual Studio with the C++ x86 tools was not found.
  exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars32.bat" >nul
if errorlevel 1 exit /b 1
if not exist artifacts mkdir artifacts

call :lua || exit /b 1
call :steam || exit /b 1
call :dll || exit /b 1
if "%CONFIG%"=="Release" (
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\make-release.ps1 || exit /b 1
)
echo Built and tested out\%CONFIG%\dinput8.dll
exit /b 0

:lua
rem Lua 5.0.2 runs the script tests. It's built with /Od because optimized builds of it crash.
if not exist out\Lua mkdir out\Lua
pushd out\Lua
cl /nologo /MT /Od /Zi /D_CRT_SECURE_NO_WARNINGS /I ..\..\third_party\lua-5.0.2\include ..\..\third_party\lua-5.0.2\src\*.c ..\..\third_party\lua-5.0.2\src\lib\*.c ..\..\third_party\lua-5.0.2\src\lua\lua.c /Fe:lua50.exe || exit /b 1
popd
for %%t in (screen_observer campaign_capture player_ready player_hold player_menu battle_exit) do (
  out\Lua\lua50.exe tests\%%t_test.lua || exit /b 1
)
exit /b 0

:steam
rem The Steam layer's unit tests, then the offline harness against a fake steam_api.dll.
if not exist out\Steam\harness mkdir out\Steam\harness
pushd out\Steam
%CXX% /O2 ..\..\tests\steam_invite_test.cpp /Fe:steam_invite_test.exe || exit /b 1
.\steam_invite_test.exe || exit /b 1
%CXX% /O2 ..\..\tests\session_test.cpp /Fe:session_test.exe || exit /b 1
.\session_test.exe || exit /b 1
cd harness
%CXX% /O2 /LD ..\..\..\tests\fake_steam_api.cpp /Fe:steam_api.dll /link ws2_32.lib || exit /b 1
set "HARNESS=/DGC_SESSION=1 /DGC_PLAYER_APPLY=1 /DGC_PLAYER_MENU=1 ..\..\..\tests\steam_invite_harness.cpp ..\..\..\src\steam_invite.cpp ..\..\..\src\diagnostics.cpp ..\..\..\src\capture_decode.cpp ..\..\..\src\capture_live.cpp ..\..\..\src\save_candidate.cpp ..\..\..\src\apply_mailbox.cpp ..\..\..\src\battle_net.cpp"
%CXX% /O2 /DGC_DEV=1 %HARNESS% /Fe:steam_invite_harness.exe /link steam_api.lib bcrypt.lib ws2_32.lib || exit /b 1
for %%s in (host "guest +connect_lobby 109775241021786432" "guest_badbuild +connect_lobby 109775241021786432" relay) do (
  .\steam_invite_harness.exe %%~s || exit /b 1
)
call :scenarios .\steam_invite_harness.exe || exit /b 1
rem The release build of the same layer, driven only from the Online menu.
%CXX% /O2 /DGC_DEV=0 %HARNESS% /Fe:steam_invite_harness_release.exe /link steam_api.lib bcrypt.lib ws2_32.lib || exit /b 1
call :scenarios .\steam_invite_harness_release.exe || exit /b 1
popd
exit /b 0

:scenarios
set "COOP=..\..\..\tests\fixtures\coop-galaxy.log"
set "VERSUS=..\..\..\tests\fixtures\versus-galaxy.log"
%1 session %COOP% || exit /b 1
for %%s in (hostgone hostends friendends friendgone hostcloses friendcloses battle) do (
  %1 %%s || exit /b 1
)
%1 hostrestart +connect_lobby 109775241021786432 || exit /b 1
%1 versus %VERSUS% || exit /b 1
%1 versusbattle || exit /b 1
%1 versusload %VERSUS% || exit /b 1
for %%s in (versushostcloses versusfriendcloses) do (
  %1 %%s || exit /b 1
)
for %%s in (coop3 coop4 coopjoin) do (
  %1 %%s %COOP% || exit /b 1
)
%1 coopback || exit /b 1
%1 battle3 || exit /b 1
exit /b 0

:dll
rem The DLL itself, with the tests that need its generated scripts or MinHook.
if not exist out\%CONFIG% mkdir out\%CONFIG%
pushd out\%CONFIG%
powershell -NoProfile -ExecutionPolicy Bypass -File ..\..\tools\embed-screen-observer.ps1 -Output screen_observer.inc || exit /b 1
rem In-process capture checking has to reproduce the recorded results.
%CXX% /O2 ..\..\src\capture_decode.cpp ..\..\src\save_candidate.cpp ..\..\tests\capture_parity.cpp /Fe:capture_parity.exe /link bcrypt.lib || exit /b 1
.\capture_parity.exe fixtures ..\..\tests\fixtures || exit /b 1
powershell -NoProfile -ExecutionPolicy Bypass -File ..\..\tools\embed-player-apply.ps1 -Output player_apply.inc || exit /b 1
powershell -NoProfile -ExecutionPolicy Bypass -File ..\..\tools\embed-player-menu.ps1 -Output player_menu.inc || exit /b 1
powershell -NoProfile -ExecutionPolicy Bypass -File ..\..\tools\embed-battle-exit.ps1 -Output battle_exit.inc || exit /b 1
rem The apply scripts run against galaxies rebuilt from two recorded sessions.
.\capture_parity.exe wire ..\..\tests\fixtures\coop-galaxy.log 0 apply-first.bin || exit /b 1
.\capture_parity.exe wire ..\..\tests\fixtures\coop-galaxy.log 2 apply-second.bin || exit /b 1
for %%w in (5 6 7 8) do (
  .\capture_parity.exe wire ..\..\tests\fixtures\versus-galaxy.log %%w apply-live-%%w.bin || exit /b 1
)
..\Lua\lua50.exe ..\..\tests\player_apply_test.lua . apply-first.bin apply-second.bin apply-live-5.bin apply-live-6.bin apply-live-7.bin apply-live-8.bin || exit /b 1
%CXX% /O2 ..\..\tests\apply_test.cpp /Fe:apply_test.exe || exit /b 1
.\apply_test.exe || exit /b 1
%CXX% /O2 /DGC_BATTLE_NET_TEST=1 ..\..\src\diagnostics.cpp ..\..\src\battle_net.cpp ..\..\tests\battle_test.cpp /Fe:battle_test.exe /link bcrypt.lib || exit /b 1
.\battle_test.exe || exit /b 1
%CXX% /O2 ..\..\tests\sha256_test.cpp /Fe:sha256_test.exe /link bcrypt.lib || exit /b 1
.\sha256_test.exe || exit /b 1
%CXX% /O2 ..\..\tests\exit_fault_test.cpp /Fe:exit_fault_test.exe || exit /b 1
.\exit_fault_test.exe || exit /b 1
%CXX% /O2 ..\..\tests\runtime_commit_test.cpp /Fe:runtime_commit_test.exe || exit /b 1
.\runtime_commit_test.exe || exit /b 1
%CXX% /O2 ..\..\tests\save_transaction_test.cpp /Fe:save_transaction_test.exe || exit /b 1
.\save_transaction_test.exe || exit /b 1
cl /nologo /W3 /MT /O2 /c ..\..\third_party\minhook\src\buffer.c ..\..\third_party\minhook\src\hook.c ..\..\third_party\minhook\src\trampoline.c ..\..\third_party\minhook\src\hde\hde32.c || exit /b 1
if defined DLL_RES (
  rc /nologo /fo %DLL_RES% ..\..\src\player.rc || exit /b 1
)
%CXX% %DLL_FLAGS% %PLAYER_FLAGS% /I . /LD ..\..\src\diagnostics.cpp ..\..\src\observers.cpp ..\..\src\frame_probe.cpp ..\..\src\proxy.cpp ..\..\src\lua_query.cpp ..\..\src\steam_invite.cpp ..\..\src\capture_decode.cpp ..\..\src\capture_live.cpp ..\..\src\save_candidate.cpp ..\..\src\apply_mailbox.cpp ..\..\src\battle_net.cpp buffer.obj hook.obj trampoline.obj hde32.obj %DLL_RES% /link %DLL_LINK% /DEF:..\..\src\dinput8.def /OUT:dinput8.dll bcrypt.lib %DLL_LIBS% || exit /b 1
%CXX% /Zi /O2 /DGC_DEV=1 /DGC_FRAME_TEST=1 ..\..\src\diagnostics.cpp ..\..\src\frame_probe.cpp ..\..\tests\frame_probe_test.cpp buffer.obj hook.obj trampoline.obj hde32.obj /Fe:frame_probe_test.exe /link bcrypt.lib || exit /b 1
.\frame_probe_test.exe || exit /b 1
.\frame_probe_test.exe game-shape || exit /b 1
.\frame_probe_test.exe shared || exit /b 1
%CXX% /O2 ..\..\tests\bots_hook_test.cpp buffer.obj hook.obj trampoline.obj hde32.obj /Fe:bots_hook_test.exe || exit /b 1
.\bots_hook_test.exe || exit /b 1
%CXX% /Zi /O2 /DGC_DEV=1 ..\..\src\diagnostics.cpp ..\..\src\lua_query.cpp ..\..\tests\lua_query_test.cpp /Fe:lua_query_test.exe /link bcrypt.lib || exit /b 1
.\lua_query_test.exe || exit /b 1
rem Imports DirectInput8Create the way the game does, so the loader picks up this folder's dinput8.dll.
%CXX% /O2 ..\..\tests\static_load_test.cpp /Fe:static_load_test.exe /link dinput8.lib || exit /b 1
.\static_load_test.exe || exit /b 1
popd
exit /b 0

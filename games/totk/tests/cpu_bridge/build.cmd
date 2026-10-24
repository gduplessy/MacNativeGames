@echo off
rem Windows x64 conformance test for runtime/cpu (jit backend), the generated NVN and HLE-import bridges and GUEST_CALL.
rem Needs Visual Studio 2022 Build Tools (MSVC x64), Python 3.14, and dynarmic built as in games/totk/spikes/x64/README.md
rem (DYNARMIC_SRC / DYNARMIC_BUILD override where it is). Then run.cmd.
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set VCVARS=
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS (echo no MSVC x64 toolset found & exit /b 1)
call "%VCVARS%" >nul 2>&1 || exit /b 1
cd /d "%~dp0"
set TOTK=..\..
set NVN=%TOTK%\third_party\d2r_gfx\gfx\nvn
set CPU=%TOTK%\runtime\cpu
if not defined DYNARMIC_SRC set DYNARMIC_SRC=%TOTK%\spikes\x64\third_party\dynarmic
if not defined DYNARMIC_BUILD set DYNARMIC_BUILD=%TOTK%\spikes\x64\build\dynarmic
set DYNLIBS=%DYNARMIC_BUILD%\src\dynarmic\dynarmic.lib %DYNARMIC_BUILD%\externals\fmt\fmt.lib %DYNARMIC_BUILD%\externals\mcl\src\mcl.lib %DYNARMIC_BUILD%\externals\zydis\Zydis.lib %DYNARMIC_BUILD%\externals\zydis\zycore\Zycore.lib
if not exist build\negative mkdir build\negative
rem the committed bridges must be what the generator makes now; the probes are test-only
python %TOTK%\tools\gen_cpu_bridge.py %NVN% --check --probes build\probes.c || exit /b 1
for /f "tokens=3" %%n in ('findstr /c:"#define NVN_BRIDGE_SLOTS" %CPU%\nvn_bridge.c') do echo #define NVN_BRIDGE_SLOTS %%n> build\slot_count.h
set INC=/I%CPU% /I%NVN%\include /Ibuild
set CFLAGS=/nologo /c /std:c17 /O2 /MD /W4 /WX /wd4100 /Zc:preprocessor %INC%
set TESTFLAGS=/nologo /std:c++20 /EHsc /O2 /MD /W3 /permissive- %INC% /I%DYNARMIC_SRC%\externals\oaknut\include
cl %CFLAGS% %CPU%\nvn_bridge.c /Fobuild\nvn_bridge.obj || exit /b 1
cl %CFLAGS% %CPU%\hle_bridge.c /Fobuild\hle_bridge.obj || exit /b 1
cl /nologo /c /std:c17 /O2 /MD /W3 %INC% build\probes.c /Fobuild\probes.obj || exit /b 1
cl %CFLAGS% /DCPU_BACKEND_JIT guest_call_test.c /Fobuild\guest_call_jit.obj || exit /b 1
cl /nologo /c /std:c++20 /EHsc /O2 /MD /W4 /WX /wd4100 /wd4324 /permissive- %INC% /I%DYNARMIC_SRC%\src /I%DYNARMIC_SRC%\externals\mcl\include %CPU%\cpu_jit.cpp /Fobuild\cpu_jit.obj || exit /b 1
set OBJS=build\cpu_jit.obj build\probes.obj build\guest_call_jit.obj
cl %TESTFLAGS% bridge_test.cpp %OBJS% build\nvn_bridge.obj build\hle_bridge.obj /Fobuild\ /Febuild\bridge_test.exe /link /HIGHENTROPYVA:NO %DYNLIBS% || exit /b 1
rem GUEST_CALL in native mode (no CPU_BACKEND_JIT): the plain cast-and-call the ARM64 runtime compiles
cl /nologo /std:c17 /O2 /MD /W4 /WX /Zc:preprocessor %INC% guest_call_test.c /Fobuild\native\ /Febuild\guest_call_native.exe || exit /b 1
rem negative control: the same test against bridges numbered by argument position (the Windows x64 rule); it must fail
python %TOTK%\tools\gen_cpu_bridge.py %NVN% --negative -o build\nvn_bridge_negative.c --hle-output build\hle_bridge_negative.c || exit /b 1
cl /nologo /c /std:c17 /O2 /MD /W3 %INC% build\nvn_bridge_negative.c /Fobuild\nvn_bridge_negative.obj || exit /b 1
cl /nologo /c /std:c17 /O2 /MD /W3 %INC% build\hle_bridge_negative.c /Fobuild\hle_bridge_negative.obj || exit /b 1
cl %TESTFLAGS% bridge_test.cpp %OBJS% build\nvn_bridge_negative.obj build\hle_bridge_negative.obj /Fobuild\negative\ /Febuild\bridge_test_negative.exe /link /HIGHENTROPYVA:NO %DYNLIBS% || exit /b 1

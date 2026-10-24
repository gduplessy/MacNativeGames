@echo off
rem Runs the conformance test, then the negative control, which must be rejected.
setlocal
cd /d "%~dp0"
build\bridge_test.exe || (echo conformance test failed & exit /b 1)
build\bridge_test_negative.exe > build\negative.txt
if not errorlevel 1 (echo negative control was NOT rejected & type build\negative.txt & exit /b 1)
findstr /b /c:"FAIL M arguments" build\negative.txt >nul || (echo negative control failed for another reason & type build\negative.txt & exit /b 1)
for /f "tokens=4 delims== " %%f in ('findstr /b /c:"checks=" build\negative.txt') do echo negative control rejected: %%f failed checks
exit /b 0

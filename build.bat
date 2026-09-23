@echo off
setlocal
cd /d "%~dp0"

if not exist bin mkdir bin

echo ========================================================
echo   Building vxe-monitor (tray + probe)
echo ========================================================

where g++ >nul 2>nul
if %ERRORLEVEL% equ 0 (
    echo [Toolchain] Using MinGW GCC/G++ ...
    g++ -O3 -mwindows -municode -s -static -std=c++17 src\main.cpp -lsetupapi -lhid -luser32 -lgdi32 -lshell32 -ladvapi32 -o bin\vxe-monitor.exe
    if errorlevel 1 goto fail
    g++ -O2 -mconsole -municode -s -static -std=c++17 tools\probe.cpp -lsetupapi -lhid -o bin\probe.exe
    if errorlevel 1 goto fail
    goto done
)

where cl >nul 2>nul
if %ERRORLEVEL% equ 0 (
    echo [Toolchain] Using MSVC CL ...
    cl /nologo /O2 /MT /EHsc /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /std:c++17 src\main.cpp /Fe:bin\vxe-monitor.exe /link /SUBSYSTEM:WINDOWS setupapi.lib hid.lib user32.lib gdi32.lib shell32.lib advapi32.lib
    if errorlevel 1 goto fail
    cl /nologo /O2 /MT /EHsc /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /std:c++17 tools\probe.cpp /Fe:bin\probe.exe /link /SUBSYSTEM:CONSOLE setupapi.lib hid.lib user32.lib
    if errorlevel 1 goto fail
    goto done
)

echo [ERROR] Neither g++ nor cl.exe was found in PATH.
exit /b 1

:fail
echo.
echo [ERROR] Compilation failed.
exit /b 1

:done
if exist bin\vxe-monitor.exe (
    echo.
    echo [SUCCESS] Build completed:
    dir bin\vxe-monitor.exe | findstr vxe-monitor.exe
    dir bin\probe.exe | findstr probe.exe
) else (
    echo.
    echo [ERROR] Compilation failed.
    exit /b 1
)

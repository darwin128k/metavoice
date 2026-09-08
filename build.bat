@echo off
setlocal
cd /d "%~dp0"
for %%I in ("%CD%") do set "MV=%%~fI\"
for %%I in ("%MV%..") do set "PARENT=%%~fI"

set VCVARS="C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars32.bat"
call %VCVARS% >nul
if errorlevel 1 (
    echo Failed to init MSVC x86 environment
    exit /b 1
)

set "BIN=%MV%build\x86\Release"
cmake -S "%MV%." -B "%BIN%" -G "Visual Studio 17 2022" -A Win32
if errorlevel 1 (
    echo CMake configure failed
    exit /b 1
)

cmake --build "%BIN%" --config Release --target MetaVoice
if errorlevel 1 (
    echo MetaVoice build failed
    exit /b 1
)

if exist "%PARENT%\cstrike\metahook\plugins" (
    copy /Y "%BIN%\Release\MetaVoice.dll" "%PARENT%\cstrike\metahook\plugins\" >nul
    echo Build OK: MetaVoice.dll -^> cstrike\metahook\plugins
) else (
    echo Build OK: %BIN%\Release\MetaVoice.dll
)
endlocal

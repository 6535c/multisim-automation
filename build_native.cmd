@echo off
setlocal
if defined MULTISIM_VCVARS32 (
    call "%MULTISIM_VCVARS32%" >nul
) else if not "%VSCMD_ARG_TGT_ARCH%"=="x86" (
    call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" >nul
)
if errorlevel 1 exit /b %errorlevel%
cl /nologo /EHsc /O2 /LD native_bridge.cpp /link user32.lib /OUT:native_bridge.dll
if errorlevel 1 exit /b %errorlevel%
cl /nologo /EHsc /O2 native_host.cpp /link user32.lib /OUT:native_host.exe
exit /b %errorlevel%

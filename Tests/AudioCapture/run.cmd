@echo off
setlocal
cd /d "%~dp0"
if not exist "..\..\build\Tests\AudioCapture" mkdir "..\..\build\Tests\AudioCapture"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
 echo Visual Studio C++ build tools and vswhere are required.
 exit /b 1
)
for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%I"
if not defined VSINSTALL (
 echo No Visual Studio installation with C++ build tools was found.
 exit /b 1
)
call "%VSINSTALL%\VC\Auxiliary\Build\vcvars32.bat" >nul
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /O2 /Gy /MD /DUNICODE /D_UNICODE /I"..\..\DLL" capture_tests.cpp /Fo"..\..\build\Tests\AudioCapture\capture_tests.obj" /Fe"..\..\build\Tests\AudioCapture\capture_tests.exe" /link /LIBPATH:"..\..\DLL\Lib\Detours" /LIBPATH:"..\..\DLL\Lib\DirectX" /OPT:REF /OPT:ICF ole32.lib user32.lib psapi.lib >"..\..\build\Tests\AudioCapture\build.log" 2>&1
if errorlevel 1 (
 type "..\..\build\Tests\AudioCapture\build.log"
 exit /b 1
)
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0run-tests.ps1"
exit /b %errorlevel%

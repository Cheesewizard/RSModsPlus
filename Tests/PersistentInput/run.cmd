@echo off
setlocal
cd /d "%~dp0"
if not exist "..\..\build\Tests\PersistentInput" mkdir "..\..\build\Tests\PersistentInput"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%I"
if not defined VSINSTALL exit /b 1
call "%VSINSTALL%\VC\Auxiliary\Build\vcvars32.bat" >nul
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /O2 /Gy /MD /DUNICODE /D_UNICODE /I"..\..\DLL" persistent_tests.cpp /Fo"..\..\build\Tests\PersistentInput\persistent_tests.obj" /Fe"..\..\build\Tests\PersistentInput\persistent_tests.exe" /link /LIBPATH:"..\..\DLL\Lib\Detours" /LIBPATH:"..\..\DLL\Lib\DirectX" /OPT:REF /OPT:ICF ole32.lib uuid.lib propsys.lib avrt.lib user32.lib psapi.lib >"..\..\build\Tests\PersistentInput\build.log" 2>&1
if errorlevel 1 (
 type "..\..\build\Tests\PersistentInput\build.log"
 exit /b 1
)
"..\..\build\Tests\PersistentInput\persistent_tests.exe"
exit /b %errorlevel%

@echo off
setlocal
cd /d "%~dp0\..\.."
set "TESTOUT=build\Tests\MlControl\%RANDOM%-%RANDOM%"
mkdir "%TESTOUT%"
for /f "usebackq tokens=*" %%I in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%I"
call "%VSINSTALL%\VC\Auxiliary\Build\vcvars32.bat" >nul
cl /nologo /std:c++17 /EHsc /O2 /MD /DUNICODE /D_UNICODE /I DLL Tests\MlControl\launcher_tests.cpp /Fo"%TESTOUT%\launcher.obj" /Fe"%TESTOUT%\launcher.exe" /link /LIBPATH:DLL\Lib\DirectX /LIBPATH:DLL\Lib\Detours user32.lib >"%TESTOUT%\native-build.log" 2>&1
if errorlevel 1 (
 type "%TESTOUT%\native-build.log"
 exit /b 1
)
"%VSINSTALL%\MSBuild\Current\Bin\Roslyn\csc.exe" /nologo /platform:x64 /out:"%TESTOUT%\ConnectionTests.exe" Tests\MlControl\ConnectionTests.cs RSModsPlus\MachineLearning\MlServiceConnection.cs
if errorlevel 1 exit /b 1
"%TESTOUT%\ConnectionTests.exe" "%CD%\%TESTOUT%\launcher.exe"
exit /b %errorlevel%

@echo off
setlocal
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "ROUTING_VS=%%i"
if not defined ROUTING_VS exit /b 1
call "%ROUTING_VS%\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
cd /d "%~dp0\..\.."
if not exist build\Tests\AudioRouting mkdir build\Tests\AudioRouting
cl /nologo /std:c++17 /EHsc /O2 /W4 /I Tests\AudioRouting\stubs Tests\AudioRouting\routing_tests.cpp /Fe:build\Tests\AudioRouting\routing_tests.exe /Fo:build\Tests\AudioRouting\routing_tests.obj /link ole32.lib uuid.lib avrt.lib advapi32.lib
if errorlevel 1 exit /b 1
"%ROUTING_VS%\MSBuild\Current\Bin\Roslyn\csc.exe" /nologo /out:build\Tests\AudioRouting\asio_config_tests.exe GUI\Audio\AsioProxySetup.cs Tests\AudioRouting\asio_config_tests.cs
if errorlevel 1 exit /b 1
"%ROUTING_VS%\MSBuild\Current\Bin\Roslyn\csc.exe" /nologo /out:build\Tests\AudioRouting\control_client_tests.exe GUI\Audio\AudioControlClient.cs Tests\AudioRouting\control_client_tests.cs
if errorlevel 1 exit /b 1
"%ROUTING_VS%\MSBuild\Current\Bin\Roslyn\csc.exe" /nologo /out:build\Tests\AudioRouting\input_mode_tests.exe GUI\Audio\AudioInputMode.cs GUI\Audio\AsioProxySetup.cs Tests\AudioRouting\input_mode_tests.cs
if errorlevel 1 exit /b 1
"%ROUTING_VS%\MSBuild\Current\Bin\Roslyn\csc.exe" /nologo /out:build\Tests\AudioRouting\output_plan_tests.exe GUI\Audio\OutputApplyPlan.cs Tests\AudioRouting\output_plan_tests.cs
if errorlevel 1 exit /b 1
powershell -NoProfile -ExecutionPolicy Bypass -File Tests\AudioRouting\run-tests.ps1
exit /b %errorlevel%

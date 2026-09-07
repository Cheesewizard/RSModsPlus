@echo off
setlocal
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "ROUTING_VS=%%i"
if not defined ROUTING_VS exit /b 1
call "%ROUTING_VS%\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
cd /d "%~dp0\..\.."
if not exist artifacts\AudioRouting mkdir artifacts\AudioRouting
cl /nologo /std:c++17 /EHsc /O2 /W4 /I Tests\AudioRouting\stubs Tests\AudioRouting\routing_tests.cpp /Fe:artifacts\AudioRouting\routing_tests.exe /Fo:artifacts\AudioRouting\routing_tests.obj /link ole32.lib uuid.lib avrt.lib
if errorlevel 1 exit /b 1
"%ROUTING_VS%\MSBuild\Current\Bin\Roslyn\csc.exe" /nologo /out:artifacts\AudioRouting\asio_config_tests.exe GUI\Audio\AsioOutputConfiguration.cs Tests\AudioRouting\asio_config_tests.cs
if errorlevel 1 exit /b 1
"%ROUTING_VS%\MSBuild\Current\Bin\Roslyn\csc.exe" /nologo /out:artifacts\AudioRouting\control_client_tests.exe GUI\Audio\AudioControlClient.cs Tests\AudioRouting\control_client_tests.cs
if errorlevel 1 exit /b 1
"%ROUTING_VS%\MSBuild\Current\Bin\Roslyn\csc.exe" /nologo /out:artifacts\AudioRouting\input_mode_tests.exe GUI\Audio\AudioInputMode.cs Tests\AudioRouting\input_mode_tests.cs
if errorlevel 1 exit /b 1
"%ROUTING_VS%\MSBuild\Current\Bin\Roslyn\csc.exe" /nologo /out:artifacts\AudioRouting\buffer_tuning_tests.exe GUI\Audio\AudioControlClient.cs GUI\Audio\AudioBufferTuner.cs Tests\AudioRouting\buffer_tuning_tests.cs
if errorlevel 1 exit /b 1
artifacts\AudioRouting\buffer_tuning_tests.exe
if errorlevel 1 exit /b 1
powershell -NoProfile -ExecutionPolicy Bypass -File Tests\AudioRouting\run-tests.ps1
exit /b %errorlevel%

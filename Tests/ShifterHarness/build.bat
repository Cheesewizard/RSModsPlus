@echo off
setlocal

where cl >nul 2>&1
if %errorlevel%==0 goto :build

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
	echo Could not find vswhere.exe. Run this from a Developer Command Prompt instead.
	pause
	exit /b 1
)

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH (
	echo Could not find a Visual Studio installation with C++ tools.
	pause
	exit /b 1
)

call "%VSPATH%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul

:build
set FLAGS=/nologo /O2 /EHsc /std:c++17 /D_CRT_SECURE_NO_WARNINGS /I . /I ..\..\DLL\Audio

cl %FLAGS% drop_pedal_latency.cpp ..\..\DLL\Audio\DelayLinePitchShifter.cpp /Fe:drop_pedal_latency.exe
if %errorlevel% neq 0 exit /b 1

cl %FLAGS% harness.cpp ..\..\DLL\Audio\DelayLinePitchShifter.cpp /Fe:harness.exe
if %errorlevel% neq 0 exit /b 1

cl %FLAGS% speaker_latency.cpp /Fe:speaker_latency.exe
if %errorlevel% neq 0 exit /b 1

cl %FLAGS% speaker_pipeline_benchmark.cpp /Fe:speaker_pipeline_benchmark.exe
if %errorlevel% neq 0 exit /b 1

echo Built drop_pedal_latency.exe, harness.exe, speaker_latency.exe and speaker_pipeline_benchmark.exe

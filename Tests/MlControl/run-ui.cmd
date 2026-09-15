@echo off
setlocal
cd /d "%~dp0\..\.."
set "TESTOUT=build\Tests\MlControl\ui-%RANDOM%-%RANDOM%"
mkdir "%TESTOUT%"
for /f "usebackq tokens=*" %%I in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.Component.MSBuild -property installationPath`) do set "VSINSTALL=%%I"
"%VSINSTALL%\MSBuild\Current\Bin\Roslyn\csc.exe" /nologo /target:exe /out:"%TESTOUT%\UiPreview.exe" /r:System.Windows.Forms.dll /r:System.Drawing.dll Tests\MlControl\UiPreview.cs GUI\UI.MlService.cs GUI\Audio\StudioTheme.cs GUI\Audio\StudioButton.cs RSModsPlus\MachineLearning\MlServiceConnection.cs
if errorlevel 1 exit /b 1
"%TESTOUT%\UiPreview.exe" "%CD%\%TESTOUT%\ml-panel.png"
exit /b %errorlevel%

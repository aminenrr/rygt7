@echo off
rem Builds RyGT7.exe (the loader) with the C# compiler that ships with Windows' .NET Framework 4.x.
rem Usage: build.cmd [output folder]
setlocal
set OUT=%~1
if "%OUT%"=="" set OUT=%~dp0
set CSC=%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\csc.exe
if not exist "%CSC%" set CSC=%WINDIR%\Microsoft.NET\Framework\v4.0.30319\csc.exe
"%CSC%" /nologo /target:winexe /optimize+ /win32icon:"%~dp0ryGT7.ico" /out:"%OUT%\RyGT7.exe" ^
    /r:System.dll /r:System.Core.dll /r:System.Drawing.dll /r:System.Windows.Forms.dll /r:System.Web.Extensions.dll /r:System.Xml.dll /r:System.IO.Compression.dll /r:System.IO.Compression.FileSystem.dll /r:System.Security.dll ^
    "%~dp0Version.cs" "%~dp0RyGTLoader.cs" "%~dp0GT7Setup.cs" "%~dp0Bindings.cs" "%~dp0ControlsPage.cs" "%~dp0PortsForm.cs" "%~dp0FeelForm.cs" "%~dp0Logging.cs" "%~dp0Wheel.cs" "%~dp0DlssDownload.cs" "%~dp0Online.cs" "%~dp0CustomEnvironment.cs" "%~dp0AdvancedPage.cs" "%~dp0Updater.cs" "%~dp0Trophies.cs" ^
    /resource:"%~dp0trophy_assets\bronze.png",trophy.bronze.png /resource:"%~dp0trophy_assets\silver.png",trophy.silver.png /resource:"%~dp0trophy_assets\gold.png",trophy.gold.png /resource:"%~dp0trophy_assets\platinum.png",trophy.platinum.png /resource:"%~dp0trophy_assets\trophy.wav",trophy.trophy.wav /resource:"%~dp0trophy_assets\platinum.wav",trophy.platinum.wav /resource:"%~dp0trophy_assets\font.ttf",trophy.font.ttf /resource:"%~dp0trophy_assets\font-LICENSE.txt",trophy.font-LICENSE.txt

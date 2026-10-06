@echo off
rem Windows setup for bbport: a window to choose the game folder and the settings, which then
rem installs MSYS2 and its packages, builds the port and writes Bloodborne.cmd and the shortcuts.
rem The game can also be installed from its .pkg files there (or with out\bbport-pkg.exe).
rem It is compiled here from tools\setup\BbportSetup.cs with the C# compiler of .NET Framework 4
rem (part of Windows 10 and 11) into out\bbport-setup.exe. Run it again to change the settings.
setlocal
set "CSC=%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\csc.exe"
if not exist "%CSC%" set "CSC=%WINDIR%\Microsoft.NET\Framework\v4.0.30319\csc.exe"
if not exist "%CSC%" (
    echo The C# compiler of .NET Framework 4 was not found: enable .NET Framework 4.8 in Windows Features.
    pause
    exit /b 1
)
if not exist "%~dp0out" mkdir "%~dp0out"
"%CSC%" /nologo /target:winexe /optimize+ /out:"%~dp0out\bbport-setup.exe" ^
    /r:System.Windows.Forms.dll /r:System.Drawing.dll /r:System.Numerics.dll ^
    "%~dp0tools\setup\BbportSetup.cs" "%~dp0tools\setup\PkgInstall.cs"
if errorlevel 1 (
    echo Compiling the setup program failed.
    pause
    exit /b 1
)
rem bbport-pkg.exe: the .pkg installer from a command line (tools\setup\PkgTool.cs).
"%CSC%" /nologo /target:exe /optimize+ /out:"%~dp0out\bbport-pkg.exe" /r:System.Numerics.dll ^
    "%~dp0tools\setup\PkgInstall.cs" "%~dp0tools\setup\PkgTool.cs"
if errorlevel 1 (
    echo Compiling bbport-pkg failed.
    pause
    exit /b 1
)
if "%~1"=="--build-only" exit /b 0
start "" "%~dp0out\bbport-setup.exe" --root "%~dp0."

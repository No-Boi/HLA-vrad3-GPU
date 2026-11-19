@echo off
rem Builds dist\: vrad3.exe, vrad3_settings.exe, vrad2.exe and oidn-shim\OpenImageDenoise.dll. Visual Studio and the Windows SDK are found automatically.
rem Optional arguments are passed through, e.g.
rem   Build.cmd -VisualStudio "D:\VS\BuildTools" -Dxc "C:\path\to\dxc.exe"
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\Build-SingleExe.ps1" %*
set "RESULT=%errorlevel%"
if "%RESULT%"=="0" (
    echo.
    echo Build succeeded: "%~dp0dist\vrad3.exe"
) else (
    echo.
    echo Build FAILED - see the messages above.
)
rem Keep the window open when started by double-click.
echo %cmdcmdline% | find /i "%~nx0" >nul && pause
exit /b %RESULT%

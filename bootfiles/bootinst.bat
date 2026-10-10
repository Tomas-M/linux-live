:; exec /bin/sh "$(dirname "$0")/bootinst.sh" "$@" #
@echo off
rem The first line is a Windows label; its shell comment tolerates CRLF on Linux.
setlocal
rem Display optional installer artwork supplied by the distribution.
if exist "%~dp0bootlogo.txt" (
    color 2F
    cls
    type "%~dp0bootlogo.txt"
)
cscript.exe //nologo "%~dp0bootinst.vbs" %*
set "result=%ERRORLEVEL%"
rem The elevated copy has its own visible console and final status.
if "%result%"=="740" exit /b 0
if /i not "%~1"=="auto" pause
exit /b %result%

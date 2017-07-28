@echo off
cls

set DISK=none
set TESTFILE=testfile.tmp

echo This file is used to determine current drive letter. It should be deleted. >\%TESTFILE%
if not exist \%TESTFILE% goto readOnly

echo Determining drive letter...
for %%d in ( C D E F G H I J K L M N O P Q R S T U V W X Y Z ) do if exist %%d:\%TESTFILE% set DISK=%%d
del \%TESTFILE%
if %DISK% == none goto DiskNotFound

rem User prompt
rem ============ BEGIN USER PROMPT ============
cls
echo ===========================================================
echo            Boot setup: MyLinux
echo ===========================================================
echo  Drive letter: %DISK%
echo ===========================================================
echo  Thank you for choosing MyLinux!  
echo  Just make sure this is not your C: drive, or else
echo  your OS (that is, Windows) will not boot!
echo.
echo Press any key to continue or press Ctrl-C to cancel...
pause >nul
rem ============ END USER PROMPT ============

echo.
echo Setting up boot record for %DISK%: Please wait...

if %OS% == Windows_NT goto setupNT
goto setup95

:setupNT
\boot\syslinux.exe -maf -d /boot/ %DISK%:
goto setupDone

:setup95
\boot\syslinux.com -maf -d /boot/ %DISK%:

:setupDone
echo Bootloader installation finished!
goto pauseit

:readOnly
echo Error: BootInstall.bat started from read-only media!
goto pauseit

:DiskNotFound
echo Error: Drive letter is non-existant!

:pauseit
echo Press any key to exit...
pause > nul

:end

@echo off
cls
echo ===============================================================================
echo.
echo                            ________.__
echo                           /   ____/^|  ^| _____  ___  ___
echo                           \____  \ ^|  ^| \__  \ \  \/  /
echo                           /       \^|  ^|__/ __ \_^>    ^<
echo                          /______  /^|____(____  /__/\_ \
echo                                 \/           \/      \/
echo.
echo ===============================================================================
echo.

set DISK=none
set BOOTFLAG=boot666s.tmp

echo This file is used to determine current drive letter. It should be deleted. >\%BOOTFLAG%
if not exist \%BOOTFLAG% goto readOnly

echo.|set /p=wait please 
for %%d in ( C D E F G H I J K L M N O P Q R S T U V W X Y Z ) do echo.|set /p=. & if exist %%d:\%BOOTFLAG% set DISK=%%d
del \%BOOTFLAG%
if %DISK% == none goto DiskNotFound

echo.
echo Setting up boot record for %DISK%:, wait please...

if %OS% == Windows_NT goto setupNT
goto setup95

:setupNT
\boot\syslinux.exe -maf -d /boot/syslinux %DISK%:
goto setupDone

:setup95
\boot\syslinux.com -maf -d /boot/syslinux %DISK%:

:setupDone
echo Installation finished.
goto pauseit

:readOnly
echo You're starting Slax installer from a read-only media, this will not work.
goto pauseit

:DiskNotFound
echo Error: can't find out current drive letter

:pauseit
echo Press any key to exit...
pause > nul

:end

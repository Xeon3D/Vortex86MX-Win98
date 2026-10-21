@echo off
rem NSET.BAT - set one R6040.SYS registry switch (string value in the
rem   adapter key Class\Net\0000 of this eBOX); takes effect at the next boot.
rem Usage: NSET name value      e.g.  NSET R6040LateAn 1
rem                                   NSET R6040Mbcr 1F1A
rem                                   NSET R6040Mbcr 012A   (default)
rem Show:  regedit /e C:\M2010\NET0000.TXT "HKEY_LOCAL_MACHINE\System\CurrentControlSet\Services\Class\Net\0000"
if "%2"=="" goto usage
if not exist C:\M2010\NUL md C:\M2010
echo REGEDIT4> C:\M2010\NSET.REG
echo.>> C:\M2010\NSET.REG
echo [HKEY_LOCAL_MACHINE\System\CurrentControlSet\Services\Class\Net\0000]>> C:\M2010\NSET.REG
echo "%1"="%2">> C:\M2010\NSET.REG
regedit /s C:\M2010\NSET.REG
echo %1 = %2 set in Class\Net\0000; reboot to apply.
goto end
:usage
echo Usage: NSET name value   (see the top of NSET.BAT)
:end

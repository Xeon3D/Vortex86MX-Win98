@echo off
rem UP.BAT - upload any file to the PC (exchange\in), e.g.  UP C:\M2010\REGBOOT.TXT
set PC=10.0.0.239
if "%1"=="" goto usage
if not exist %1 goto missing
echo open %PC%> C:\M2010\up.scr
echo user anonymous ebox@>> C:\M2010\up.scr
echo binary>> C:\M2010\up.scr
echo cd in>> C:\M2010\up.scr
echo put %1>> C:\M2010\up.scr
echo bye>> C:\M2010\up.scr
ftp -n -s:C:\M2010\up.scr > nul
echo Uploaded %1
goto end
:missing
echo *** %1 not found
goto end
:usage
echo Usage: UP file   (uploads it to the PC)
:end

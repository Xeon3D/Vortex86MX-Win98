@echo off
rem BIGTEST.BAT - network stress test: download BIGTEST.BIN (32 MB random
rem   data) from the PC and upload it straight back as BIGBACK.BIN; the PC
rem   compares the two byte for byte. Shows the time before and after.
set PC=10.0.0.239
if not exist C:\M2010\NUL md C:\M2010
C:
cd \M2010
if exist BIGTEST.BIN del BIGTEST.BIN
echo open %PC%> big.scr
echo user anonymous ebox@>> big.scr
echo binary>> big.scr
echo cd out>> big.scr
echo get BIGTEST.BIN>> big.scr
echo cd ..>> big.scr
echo cd in>> big.scr
echo put BIGTEST.BIN BIGBACK.BIN>> big.scr
echo bye>> big.scr
echo Start:
echo.| time
ftp -n -s:big.scr
echo End:
echo.| time
dir BIGTEST.BIN | find "BIGTEST"
echo (should be 33.554.432 bytes) - tell Claude the two times

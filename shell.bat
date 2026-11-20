@echo off
REM Interactive SQL shell in the terminal (no browser, no Python needed)
if not exist engine\build\pagedb.exe call build.bat
if not exist data mkdir data
engine\build\pagedb.exe --cli --db data\shell.db --frames 16 --policy LRU

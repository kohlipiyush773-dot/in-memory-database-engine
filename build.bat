@echo off
REM Builds the PageDB engine and the test program with MinGW g++ (works with old g++ 6.x too)
setlocal
if not exist engine\build mkdir engine\build
set SRC=engine\src\slotted_page.cpp engine\src\disk_manager.cpp engine\src\log_manager.cpp engine\src\replacers.cpp engine\src\buffer_pool_manager.cpp engine\src\btree.cpp engine\src\sql.cpp engine\src\database.cpp engine\src\executor.cpp engine\src\simulator.cpp
echo Compiling the engine ...
g++ -std=gnu++14 -O2 -Wall -Iengine\include %SRC% engine\src\protocol.cpp engine\src\main.cpp -o engine\build\pagedb.exe
if errorlevel 1 goto fail
echo Compiling the tests ...
g++ -std=gnu++14 -O2 -Wall -Iengine\include %SRC% engine\tests\test_engine.cpp -o engine\build\test_engine.exe
if errorlevel 1 goto fail
echo.
echo BUILD OK.   Run tests:  engine\build\test_engine.exe      Start the app:  run.bat
exit /b 0
:fail
echo.
echo BUILD FAILED - copy the error text and send it to me.
exit /b 1

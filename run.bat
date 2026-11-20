@echo off
REM Starts the PageDB web app and opens http://127.0.0.1:8080
if not exist engine\build\pagedb.exe call build.bat
where python >nul 2>nul
if %errorlevel%==0 (
  python server\server.py %*
) else (
  py -3 server\server.py %*
)

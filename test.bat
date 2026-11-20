@echo off
if not exist engine\build\test_engine.exe call build.bat
pushd engine\build
test_engine.exe
popd

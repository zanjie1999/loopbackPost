@echo off
setlocal
cl /nologo /EHsc /std:c++17 loopbackPost.cpp /link winhttp.lib ole32.lib uuid.lib
if errorlevel 1 exit /b %errorlevel%
echo.
echo Built: loopbackPost.exe

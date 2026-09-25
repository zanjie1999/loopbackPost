@echo off
setlocal
where cl >nul 2>nul
if errorlevel 1 (
  echo MSVC environment is not initialized.
  echo Run this from a Visual Studio Developer Command Prompt.
  exit /b 1
)
cl /nologo /EHsc /std:c++17 /O2 /MT loopbackPost.cpp /Fe:loopbackPost.exe /link winhttp.lib ole32.lib uuid.lib
endlocal

@echo off
REM 构建 hbut_jw.dll（Release x64）。
REM 默认按 TabMind 源码树布局走：plugins\hbut_jw\ 与 ..\..\include\plugin_manager.hpp
REM 换机器只需改下面三行 MSVC / SDK 路径。
setlocal

set MSVC=D:\vs studio\a\VC\Tools\MSVC\14.44.35207
set SDK=D:\Windows Kits\10
set SDKV=10.0.26100.0

if "%TABMIND_INCLUDE%"=="" set TABMIND_INCLUDE=%~dp0..\..\include

set INCLUDE=%MSVC%\include;%SDK%\Include\%SDKV%\ucrt;%SDK%\Include\%SDKV%\um;%SDK%\Include\%SDKV%\shared
set LIB=%MSVC%\lib\x64;%SDK%\Lib\%SDKV%\ucrt\x64;%SDK%\Lib\%SDKV%\um\x64
set CL=%MSVC%\bin\Hostx64\x64\cl.exe

cd /d "%~dp0"
if not exist obj mkdir obj

echo 头文件目录: %TABMIND_INCLUDE%

"%CL%" /nologo /LD /std:c++17 /O2 /MT /EHsc /utf-8 ^
 /D NDEBUG /D _WINDOWS /D _USRDLL /D NOMINMAX /D _CRT_SECURE_NO_WARNINGS /D UNICODE /D _UNICODE ^
 /I "%TABMIND_INCLUDE%" /Foobj\ /Fdobj\hbut_jw.pdb /Fe:hbut_jw.dll ^
 src\jw_util.cpp src\jw_aes.cpp src\jw_http.cpp src\jw_hbut.cpp src\jw_render.cpp src\dllmain.cpp ^
 /link /SUBSYSTEM:WINDOWS /DLL ^
 gdiplus.lib winhttp.lib ole32.lib user32.lib gdi32.lib advapi32.lib

if %errorlevel%==0 (
  echo.
  echo BUILD OK: %~dp0hbut_jw.dll
  echo 把 hbut_jw.dll 复制到 TabMind 的 plugins\ 目录后重启程序即可。
) else (
  echo BUILD FAILED
)
endlocal

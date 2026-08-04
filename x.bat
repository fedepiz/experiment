@echo off
rem x.bat -- project driver, mirroring x.sh for cmd/PowerShell. Commands:
rem build [debug|release], run [--debug|--release] [args...], clean.
rem The flags mirror x.sh exactly -- see the comments there for the why.
setlocal
cd /d "%~dp0"

rem clang targeting msvc; override by setting CC before calling
if not defined CC set CC=clang

set COMMON=-std=c11 -D_DEFAULT_SOURCE -Isrc -Wall -Wextra -Wundef -Wno-unused-function -Werror=implicit-function-declaration -Werror=implicit-int -Werror=incompatible-pointer-types -Werror=int-conversion -Werror=pointer-sign -Werror=return-type
set DEBUG=-g -O0 -DBUILD_DEBUG=1
set RELEASE=-g -O2 -DBUILD_DEBUG=0
set LIBS=-luser32 -lgdi32 -lopengl32

if "%~1"=="" goto cmd_build
if "%~1"=="build" goto cmd_build
if "%~1"=="run" goto cmd_run
if "%~1"=="clean" goto cmd_clean
goto usage

:cmd_build
set mode=%~2
if not defined mode set mode=debug
call :do_build %mode% || exit /b 1
exit /b 0

:cmd_run
rem unity build is fast enough to always rebuild before running
shift
set mode=debug
if /i "%~1"=="--release" (set mode=release& shift)
if /i "%~1"=="--debug" (set mode=debug& shift)
call :do_build %mode% || exit /b 1
rem forward whatever args remain (batch has no clean "rest of %*")
set args=
:run_collect
if "%~1"=="" goto run_go
set args=%args% %1
shift
goto run_collect
:run_go
build\app.exe%args%
exit /b %errorlevel%

:cmd_clean
if exist build rmdir /s /q build
echo removed build\
exit /b 0

:do_build
if "%~1"=="debug" (set flags=%COMMON% %DEBUG%) else if "%~1"=="release" (set flags=%COMMON% %RELEASE%) else goto usage
if not exist build mkdir build
%CC% %flags% src\main.c -o build\app.exe %LIBS%
if errorlevel 1 exit /b 1
echo built build\app.exe (%~1)
exit /b 0

:usage
echo usage: %~nx0 {build [debug^|release] ^| run [args...] ^| clean} 1>&2
exit /b 1

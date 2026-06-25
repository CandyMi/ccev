@echo off
REM init-deps.cmd -- Re-initialize or repair git submodules for ccev (Windows CMD)
REM
REM Usage:
REM   scripts\init-deps.cmd          Interactive (prompts before wiping)
REM   scripts\init-deps.cmd -f       Force, no prompt
REM
REM Edit .gitmodules FIRST if you forked to a different git server.

setlocal enabledelayedexpansion

REM -- check prerequisites --
where git >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo [init-deps] ERROR: git not found
    exit /b 1
)

if not exist ".gitmodules" (
    echo [init-deps] ERROR: no .gitmodules found -- run from ccev project root
    exit /b 1
)

REM -- prompt --
if "%1" NEQ "-f" (
    echo ============================================
    echo   ccev -- deps re-initialization
    echo.
    echo   This will DELETE deps/ and re-clone all
    echo   submodules from the URLs in .gitmodules.
    echo.
    echo   If you forked the deps to your own git
    echo   server, edit .gitmodules FIRST, then
    echo   re-run this script.
    echo ============================================
    set /p REPLY="Proceed? [y/N] "
    if /i "!REPLY!" NEQ "y" (
        if /i "!REPLY!" NEQ "yes" (
            echo [init-deps] cancelled
            exit /b 0
        )
    )
)

REM -- clean --
echo [init-deps] removing deps/ ...
rmdir /s /q deps\epoll 2>nul
rmdir /s /q deps\ccalg 2>nul
rmdir /s /q deps\ccsocket 2>nul

REM -- re-init --
echo [init-deps] initializing submodules ...
git submodule update --init --recursive --force

REM -- verify --
set MISSING=
if not exist "deps\epoll\README.md"     set MISSING=%MISSING% epoll
if not exist "deps\ccalg\README.md"     set MISSING=%MISSING% ccalg
if not exist "deps\ccsocket\README.md"  set MISSING=%MISSING% ccsocket

if defined MISSING (
    echo [init-deps] ERROR: submodule(s) missing: %MISSING%
    echo [init-deps]   Check your git server URLs in .gitmodules
    echo [init-deps]   or restore connectivity and re-run.
    exit /b 1
)

echo [init-deps] OK -- all submodules ready
exit /b 0

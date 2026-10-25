@echo off
setlocal

rem ---------------------------------------------------------------
rem  lolview practice tool launcher
rem  Double-click to play, or pass extra arguments, e.g.:
rem      run.bat --champion Ahri --pet durian_miner --map
rem      run.bat --capture shot.png --capture-after 1.0
rem ---------------------------------------------------------------

set "ROOT=%~dp0"
set "EXE=%ROOT%build\lolview_sandbox.exe"
rem Works if the script is copied elsewhere: fall back to the default checkout.
if not exist "%EXE%" set "EXE=H:\projects\LOL\build\lolview_sandbox.exe"

rem Game install: edit this line, or set LOLVIEW_GAME_DIR before running.
if not defined LOLVIEW_GAME_DIR set "LOLVIEW_GAME_DIR=H:\Riot Games\League of Legends"

if not exist "%EXE%" (
    echo.
    echo   Build missing: %EXE%
    echo   Configure and build first:
    echo       cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=g++ -DCMAKE_BUILD_TYPE=Release
    echo       cmake --build build
    echo.
    pause
    exit /b 1
)

if not exist "%LOLVIEW_GAME_DIR%\Game" (
    echo.
    echo   League of Legends not found at:
    echo       %LOLVIEW_GAME_DIR%
    echo   Set LOLVIEW_GAME_DIR to your install, or edit this file.
    echo.
    pause
    exit /b 1
)

echo Launching lolview practice tool...
echo   install : %LOLVIEW_GAME_DIR%
echo.

start "" "%EXE%" --game-dir "%LOLVIEW_GAME_DIR%" --champion Ahri --pet durian_miner --map --mirror-x --vfx-bin aa1b62a07d7b6785 %*
exit /b 0

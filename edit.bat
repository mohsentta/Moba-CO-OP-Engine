@echo off
setlocal

rem ---------------------------------------------------------------
rem  Walkable-path editor  (F3 tool, no server needed)
rem
rem  Double-click to open the editor on the Rift.
rem
rem  In game:
rem    left click    add a polygon vertex
rem    Tab           switch walkable (cyan) / obstacle (red)
rem    Enter         close the current polygon
rem    Backspace     remove the last vertex
rem    F5            save scripts\paths.txt
rem    F3            leave the path editor
rem    F2            map-piece editor
rem    ESC           quit
rem
rem  Extra arguments are passed through, e.g.:
rem      edit.bat --champion Ahri --map
rem      edit.bat --champion Jinx --archive Jinx.wad --map
rem ---------------------------------------------------------------

set "ROOT=%~dp0"
set "EXE=%ROOT%build\lolview_sandbox.exe"
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

echo Opening the walkable-path editor...
echo   install : %LOLVIEW_GAME_DIR%
echo   saves   : %ROOT%scripts\paths.txt
echo.
echo   left click = vertex   Tab = red/green   Enter = close   F5 = save   F3 = exit mode
echo.

start "" "%EXE%" --game-dir "%LOLVIEW_GAME_DIR%" --champion Jinx --archive Jinx.wad --map --path-editor %*
exit /b 0

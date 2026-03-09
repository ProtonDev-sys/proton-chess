@echo off
setlocal
cd /d "%~dp0"

echo Play Against Proton Chess
echo.

set "COLOR="
set /p COLOR=Play as [white/black] [white]:
if not defined COLOR set "COLOR=white"

set "SECONDS="
set /p SECONDS=Seconds per engine move [0.2]:
if not defined SECONDS set "SECONDS=0.2"

set "PGN="
set /p PGN=PGN output path [out\human_vs_proton.pgn]:
if not defined PGN set "PGN=out\human_vs_proton.pgn"

echo.
echo Starting play UI...
echo   Color: %COLOR%
echo   Seconds per engine move: %SECONDS%
echo   PGN: %PGN%
echo.

python ".\tools\play_against_engine.py" ".\build\proton_chess.exe" --human-color %COLOR% --seconds %SECONDS% --pgn "%PGN%" --engine-name ProtonChess

if errorlevel 1 (
    echo.
    echo Launcher failed.
    pause
    exit /b 1
)

echo.
echo Session ended.
pause

@echo off
setlocal
cd /d "%~dp0"

echo Proton Chess vs Stockfish Launcher
echo.

set "GAMES="
set /p GAMES=Number of games [2]:
if not defined GAMES set "GAMES=2"

set "SECONDS="
set /p SECONDS=Seconds per move [0.2]:
if not defined SECONDS set "SECONDS=0.2"

set "ELO="
set /p ELO=Stockfish Elo [1800]:
if not defined ELO set "ELO=1800"

set "PGN="
set /p PGN=PGN output path [out\watched_match.pgn]:
if not defined PGN set "PGN=out\watched_match.pgn"

echo.
echo Starting visual match...
echo   Games: %GAMES%
echo   Seconds per move: %SECONDS%
echo   Stockfish Elo: %ELO%
echo   PGN: %PGN%
echo.

powershell -ExecutionPolicy Bypass -File ".\tools\watch_stockfish.ps1" -Games %GAMES% -Seconds %SECONDS% -Elo %ELO% -Pgn "%PGN%"

if errorlevel 1 (
    echo.
    echo Launcher failed.
    pause
    exit /b 1
)

echo.
echo Match window closed.
pause

param(
    [int]$Games = 2,
    [double]$Seconds = 0.2,
    [string]$Pgn = "",
    [int]$Elo = 1800
)

$root = Split-Path -Parent $PSScriptRoot
$proton = Join-Path $root "build\proton_chess.exe"
$stockfish = Join-Path $root "external\bin\stockfish18\stockfish\stockfish-windows-x86-64.exe"
$watcher = Join-Path $PSScriptRoot "watch_match.py"

if (-not (Test-Path $proton)) {
    throw "Missing Proton engine at $proton"
}

if (-not (Test-Path $stockfish)) {
    throw "Missing Stockfish engine at $stockfish"
}

$arguments = @(
    $watcher,
    $proton,
    $stockfish,
    "--games", $Games,
    "--seconds", $Seconds,
    "--opponent-name", "Stockfish18",
    "--opponent-option", "UCI_LimitStrength=true",
    "--opponent-option", "UCI_Elo=$Elo"
)

if ($Pgn) {
    $arguments += @("--pgn", $Pgn)
}

python @arguments

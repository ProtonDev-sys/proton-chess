# Proton Chess

Proton Chess is a native C++20 UCI engine with selectable strength and an explicit human-style move-selection mode. Its search remains tactical and deterministic when requested, while human mode introduces bounded variety only among moves that survive a fresh verification search.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The executable is `build/proton_chess` on Linux and `build/Release/proton_chess.exe` on Windows.

## Human-style play

Use the engine through any UCI-compatible chess interface. The direct controls are:

```text
setoption name HumanStyle value true
setoption name HumanSkill value 20
setoption name HumanMaxLossCp value 12
setoption name HumanSeed value 1
```

`HumanSkill` ranges from 0 to 20. Lower values widen the set of acceptable alternatives. `HumanMaxLossCp` sets the intentional centipawn-loss ceiling at skill 20, and `HumanSeed` makes move variety reproducible.

For standard UCI strength limiting, use:

```text
setoption name UCI_LimitStrength value true
setoption name UCI_Elo value 2200
```

The supported UCI Elo range is 800–3000. The Elo control maps to the same bounded human selector rather than merely cutting search depth.

Human mode does not blindly add noise. It preserves forced mates, filters alternatives against the configured loss allowance, and confirms a sampled candidate with a separate restricted search before returning it. The style policy favours normal development, castling, central pawn play, phase-appropriate king activity, and tactical moves when they are justified.

## Validation and benchmarking

The repository includes native tests, UCI protocol smoke tests, perft regressions, legal move-generation cross-checks, deterministic fixed-search comparison tooling, and paired engine-match tooling under `tools/`.

A passing test suite proves correctness of the covered invariants; it is not by itself an Elo claim. Strength changes should be evaluated with the pinned paired-search and colour-swapped match protocols described in `matches/README.md`.

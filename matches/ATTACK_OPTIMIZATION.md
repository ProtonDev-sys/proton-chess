# Sliding-attack optimization: evidence and limitations

Baseline: `5442524f1b8f27bac716a35b2550163c210cd0c1`.

The candidate removes blocker-dependent branches from sliding attacks without changing search, evaluation, or human-style selection. Increasing rays use unsigned `b ^ (b - 1)` to include squares through the nearest blocker (all bits when no blocker exists). Decreasing rays use a bit-zero sentinel; every decreasing ray from square zero is empty. No architecture-specific instructions or generated magic tables are required.

## Correctness

`tests/test_attacks.cpp` uses an independent coordinate-based ray scanner. It checks every blocker subset of each ray and each rook/bishop attack mask, including edge blockers and variants with all off-ray squares occupied, plus 100,000 randomized queen attacks. Checks remain active in Release builds.

Local results: **1,230,900 exhaustive/random cases passed** both with portable `-O3` and with AddressSanitizer/UndefinedBehaviorSanitizer (`-O1 -g -fsanitize=address,undefined`). These are attack tests, not a claim that the entire engine test suite ran locally. CTest and the existing Linux/Windows validation workflow cover integration.

## Performance

Reproduce the standalone benchmark:

```sh
c++ -std=c++20 -O3 -Isrc tools/benchmark_attacks.cpp -o benchmark-attacks
./benchmark-attacks
```

It compares the exact baseline algorithm with the candidate using identical inputs, warmup, nine alternating-order trials, medians, and matching nontrivial rolling checksums. Local AMD EPYC 9V74 portable-build results:

| Occupancy | Baseline median ms | Candidate median ms | Speedup |
|---|---:|---:|---:|
| Empty | 6.478 | 12.604 | 0.514x |
| 12.5% | 40.110 | 12.493 | 3.211x |
| 25% | 52.183 | 12.549 | 4.158x |
| 50% | 36.709 | 12.286 | 2.988x |
| Full | 17.572 | 12.607 | 1.394x |

**The empty-board microbenchmark regresses.** These figures are not whole-engine speedups, NPS gains, or Elo estimates. The separate `Attack optimization evidence` workflow builds candidate and PR base and runs the existing deterministic search-comparison tool over the repository opening suite. Inspect that report before deciding whether to merge.

## Stockfish target

Stockfish 19 was released on September 5, 2026. `tools/fetch_stockfish19.py` downloads and verifies the immutable official Linux x86-64 reference archive, retaining an external executable and a hash manifest rather than incorporating Stockfish code into Proton. The source is the [official release](https://github.com/official-stockfish/Stockfish/releases/tag/sf_19).

The existing Stockfish 18 Elo-3000 certification deliberately enables `UCI_LimitStrength`; it is not an unrestricted Stockfish 19 benchmark. A legitimate new target must use the exact official reference, strength limiting disabled, equal clocks/resources, 50 distinct openings with colours swapped, complete replayable results, and separate reporting of a 100-game match win versus statistical evidence of superiority.

**This PR does not demonstrate a win against Stockfish 19.** It also does not introduce a trained human-move model or claim that the existing heuristic human-style selector is a calibrated human-likeness probability. Human move quality and human move likelihood require independent measurements on held-out human games; forcing unusual moves or adding evaluation noise is not evidence of human likeness.

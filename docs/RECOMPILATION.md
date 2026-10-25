# Recompilation

> [!NOTE]
> [Documentation](README.md). Progress is tracked on the wiki: [Recompilation status](https://github.com/bloodborne-native/bloodborne-recomp/wiki/Recompilation-status).
> The game side (loading the recompiled library, recording calls) is in
> [bloodborne-runtime](https://github.com/bloodborne-native/bloodborne-runtime); the Mac app in
> [bloodborne-mac](https://github.com/bloodborne-native/bloodborne-mac).

Goal: the game's own code running as native arm64 code, compiled ahead of time instead of
translated while the game runs, and in the end the engine's graphics drawn with each platform's
own GPU API (Vulkan, Metal) instead of PS4 command buffers translated while the game runs.

The way there is a **static recompiler**: a tool that runs on the player's Mac, reads *their own*
`eboot.bin` and writes the game's functions out as C, which clang compiles into a native library
the game loads. This project publishes the recompiler, never the game's code, as *Unleashed
Recompiled* and N64Recomp do.

## What is published

| Published (this repository) | Never published |
|---|---|
| The recompiler, its runtime, the translator | The game's code |
| Record and replay, profiling and analysis tools | |
| Annotations: function names, signatures, struct layouts | |

The recompiled code is produced on the player's machine from their copy, and stays there.

## Principles

1. **Only FromSoftware's code first.** Havok, FMOD, Scaleform, YEBIS, Lua and zlib are licensed
   libraries; they can be recompiled like the rest later, the game's and engine's code comes first.
2. **Always playable.** Recompiled functions replace the translated ones one by one; anything not
   recompiled runs in the translator, which is also the fallback inside a generated function.
3. **The game's own data layouts.** Guest memory is host memory and x86-64 and arm64 share the
   LP64 layout rules: generated code works on the game's live objects.
4. **Checked by behaviour.** Calls recorded during play are replayed against the generated code;
   results, memory writes and calls must match exactly.
5. **Behind switches.** Generated functions can be turned off one by one, so a wrong one is found
   by bisection.

## Milestones

| | Goal | Status |
|---|---|---|
| R0 | Groundwork: function table, library labels, CPU profile, record and replay, native functions loaded into the game and called from translated code | Done |
| R1 | Recompiler prototype: generated C for the recorded functions, checked by replay | Done |
| R2 | Coverage: every function generated; the game runs recompiled, the translator as fallback | Done |
| R3 | Speed: integer, flags and vector instructions in C; memory ordering relaxed where safe | In progress |
| R4 | Annotations: names, signatures and structs make the generated C readable | |
| R5 | The engine's graphics on each platform's own GPU API: [native renderer](https://github.com/bloodborne-native/bloodborne-runtime/blob/main/docs/NATIVE_RENDERER.md) (bloodborne-runtime) | N1 closed; N5 (Metal backend): the frame draws through Metal behind an experimental renderer option, 550 pipelines built, 0 failed |

## Progress

*2026-10-24.* The game's executable has 241,821 functions, about 11.3 million instructions.

| | Progress | |
|---|---|---|
| Functions recompiled, running in the game | `████████████████████` 99.6% | 240,973 of 241,821 |
| Instructions recompiled | `███████████████████░` 95.3% | 10,743,599 of 11,268,132 |
| Instructions in plain C (of those recompiled) | `████████████████████` 99.1% | 10,641,693 of 10,743,599 |
| CPU time recompiled, against the translator | `███████████████████░` 94% | 373% of one core against 398% (1st Floor Sickroom), 387% against 409% (Hunter's Dream) |
| Functions checked call by call (recordings) | `░░░░░░░░░░░░░░░░░░░░` 0.4% | 933 of 241,821 |
| Functions labelled by library (2026-10-24) | `█████████████░░░░░░░` 62.7% | 151,562 of 241,821 |

Every function is generated; the ones left to the translator are the 660 import stubs, the 130
functions that call `setjmp`, and 58 that the game's patches (56) or the runtime's live switches
(2) change. Instructions recompiled count what control flow reaches; the rest is padding and code
nothing reaches. Every recompiled function can run in the game; the call-by-call checks cover the
busiest, and the fuzzing covers every instruction form the generator handles.

## The recompiler

`tools/recomp/bbrecomp.c` reads `eboot.bin`, takes functions from the function table
(`tools/recomp/scan.c`), and writes one C function per game function against `src/recomp/rc.h`;
`tools/recomp/recomp.sh` compiles them into a library, one file per 128 KiB of the image, compiling
again only the files whose C changed:

- The code is decoded by following its control flow from the entry. Jumps become `goto`s (labels
  only where something jumps); returns become C returns. Calls of other recompiled functions are
  direct C calls; other calls go through the runtime.
- Guest registers live in local arrays: the 16 general-purpose ones, and the vector registers
  the function's instructions name. They go back to the CPU state around calls, interpreted
  instructions and returns. The flags are computed lazily (clang drops what nothing reads).
- Memory accesses keep x86's ordering between threads (acquire/release), as the translator's do,
  the stack's too: the game's jobs write into their creator's stack.
- In C: moves, integer arithmetic and logic (with carry), shifts and rotates, multiplies and
  divisions, bit tests, scans and counts, BMI1, byte swaps, `rdtsc`, conditional sets and moves,
  exchanges, locked operations (compare-and-exchange loops), and the common SSE/AVX moves,
  arithmetic, dot products, blends, inserts and extracts, conversions, bitwise operations,
  compares, shifts, shuffles, packs, unpacks and rounding, each as the interpreter does
  it. A division that faults runs in the interpreter, as does anything else, one instruction at a
  time, so the output is complete from the start.
- An indirect jump to an unknown place goes back to the translator; a jump out of the function is
  a tail call.
- Left to the translator: the import stubs (one jump each to the PS4 libraries) and the functions
  that call `setjmp` (its return point must be guest code that can be resumed).
- A call the runtime runs in the translator gets a return address of its own; when a `longjmp` or
  an exception leaves several of them at once, the runtime goes back to the right one on the host
  as well.

The game loads the library (`BB_RECOMP_LIB`) after its patches: a function whose code changed since
it was generated (a game patch, a hook: `bbcpu_recomp_hash`) stays with the translator, and so
does one holding code the runtime rewrites while the game runs (`bbcpu_recomp_exclude`: the live
effect switches).
`BB_RECOMP_OFF=OFFSET,...` (or `@FILE`) switches chosen ones off, `BB_RECOMP_ONLY` keeps only the
listed ones (bisecting), `BB_RECOMP_THREADS` / `BB_RECOMP_NOT_THREADS`
limit them to some threads (by name), `BB_RECOMP_NODIRECT=1` sends every call through the
translator. Translated code calls recompiled functions directly.

Two checks: the **recordings** (whole functions, real calls) and **per-instruction fuzzing**
(`tools/recomp/fuzz.sh`: every distinct instruction encoding in the game, at most 30 per mnemonic,
run from random states through the interpreter and through its generated C).

R1 results (2026-10-10): the 24 recorded functions (2,508 instructions, 99% in C) match every
recorded call exactly as the originals do; in the game they run at the translator's speed (61.4 FPS
against 61.9, up to 1.7 million calls a second). Fuzzing: 3,443 instruction forms, 200 trials each,
no differences (a planted mistake in one flag fails 40).

R2 so far (2026-10-10): the 933 busiest functions (222,285 instructions, 99.4% in C) run in the
game on every thread at the translator's speed (62.1 FPS against 61.9). Fuzzing: 4,892 instruction
forms, 200 trials each, no differences. Recordings, 40 calls per function: 911 of 933 functions
match every call; the other 22 also fail when the original runs (they read the time stamp counter
or memory other threads change). What the checks found along the way:

- The interpreter's `lock cmpxchg` reported the value of a separate load after a failed exchange:
  when another thread had changed the value back, a failed exchange looked like a successful one,
  and two threads took the same lock. In the game this was a heap panic during save load
  (`DLRegularHeap.cpp(710)`); recompiled code, faster on some threads, made it frequent.
- The recorder missed `vmaskmovdqu`'s write (its address is implicit, in rdi).
- Speed: a shared counter of recompiled calls (every thread adding to it) and the runtime between
  recompiled functions cost 11 FPS; the instructions left to the interpreter, 7 more.

Coverage (2026-10-10): the unwind tables list 162,959 functions; leaf functions often have none.
The scanner now also follows direct calls, jumps, code addresses taken with `lea` and the code
pointers relocations put into data (vtables): 241,821 functions, and every block the game ran on a
3-minute route through the Hunter's Dream (walking, fighting, menus) is inside one. All 49,095
functions that route ran (3.06 million instructions, 99.8% in C) build in 3.5 minutes into a 98 MB
library; 49,071 of them run recompiled in the game (the rest changed by its patches) at the
translator's speed, 59.1 FPS, about 12 million recompiled calls a second. On the way:

- Recompiled import stubs put the runtime's return address on the stack, which `setjmp` saved; a
  later `longjmp` (the game's PNG reader) came back into a frame that was gone. The stubs and the
  40 functions that call `setjmp` stay with the translator.
- Every call the runtime ran in the translator returned to the same address: after a `longjmp` the
  innermost one stopped where an outer one should have.

R2 done (2026-10-10): every function generated, 241,031 (the 660 import stubs and the 130 that
call `setjmp` stay with the translator), 10.48 million instructions, 98.8% in C; the whole game
builds in 11.5 minutes into a 404 MB library. 240,995 functions run recompiled in the game (36 the
game's patches changed stay with the translator) at 55.2 FPS against ~60 with the translator
alone. Calls of imports the runtime provides go from recompiled code straight to them (2.4 million
a second); the runtime now provides `strcmp`, `ldiv`, `powf` and `modff` too, which were the
busiest calls left in the PS4 libc's own code. 1.45 million calls a second still enter the
translator: what is left for R3, with the hot code spread through the large library.

## Tools

All write to `out/recomp/`.

| Tool | What it does |
|---|---|
| `tools/recomp/scan.c` | Every function: the unwind tables (exact start and size), then what calls, jumps and code pointers reach (decoded by following control flow); `functions.tsv`, `calls.tsv`, `slots.tsv`, `strings.tsv` |
| `tools/recomp/pointers.py` | The code addresses relocations put into data (`pointers.txt`), for `scan.c`, and where each is stored (`pointer_slots.tsv`), for `label.py` |
| `tools/recomp/imports.py` | The import stubs by name (`imports.tsv`) |
| `tools/recomp/label.py` | The library of each function (`modules.tsv`), from source paths and messages, spread to neighbours, vtables, callers and callees (`--check`: their agreement) |
| `tools/recomp/profile.py` | CPU time per function and library from a macOS `sample` (`profile.tsv`) |
| `tools/recomp/inspect.sh` | Ghidra's view of chosen functions (`c/<offset>.c`, `.s`), for analysis and annotations |
| `BB_RECORD` (`src/cpu/record.c`) | Calls of chosen functions recorded in the game (`records/<offset>.rec`) |
| `tools/recomp/replay.sh` | Recorded calls replayed and checked (`--recomp LIB`: the generated code) |
| `tools/recomp/recomp.sh` | Generated C for chosen functions, compiled (`librecomp.dylib`; `BB_RECOMP_LIB`) |
| `tools/recomp/fuzz.sh` | The generator's C for every instruction form, fuzzed against the interpreter |

```sh
./build.sh                          # Zydis into deps/, out/libbbcpu.a, out/recomp/scan and bbrecomp
GAME=/path/to/CUSA03173  # the game folder (1.09)
python3 tools/recomp/ps4elf.py "$GAME/eboot.bin" out/eboot.elf
python3 tools/recomp/pointers.py "$GAME/eboot.bin" out/recomp/pointers.txt
out/recomp/scan out/eboot.elf out/recomp out/recomp/pointers.txt
python3 tools/recomp/imports.py "$GAME/eboot.bin" out/recomp
python3 tools/recomp/label.py out/recomp

# Generate and compile functions (or every one: tail -n +2 out/recomp/functions.tsv | cut -f1).
tools/recomp/recomp.sh --file LIST          # out/recomp/librecomp.dylib; the game: BB_RECOMP_LIB
tools/recomp/fuzz.sh                        # every instruction form, generated C against the interpreter

# In the game: from a bloodborne-runtime checkout (this repository is its recomp/), with its
# tools/mac_bench.sh. A profile: 5 s of every thread, then the main thread's functions.
SAMPLE=1 tools/mac_bench.sh prof BB_JIT_MAP=$PWD/out/jit.map               # in the runtime
python3 recomp/tools/recomp/profile.py out/bench_prof_*.sample.txt out/jit.map out/bench_prof_*.log Thread_<id>

brew install ghidra
tools/recomp/inspect.sh 0x21b8710

# Record calls (OFFSET:SIZE from functions.tsv) in the game, from the runtime, and replay them here.
tools/mac_bench.sh rec BB_RECORD=0x22928a0:247,0x227e230:579 BB_RECORD_EVERY=7 \
    BB_RECORD_DIR=$PWD/recomp/out/recomp/records                             # in the runtime
tools/recomp/replay.sh out/recomp/records/0x22928a0.rec                      # here
```

## Record and replay

With `BB_RECORD`, the translator leaves the chosen functions' first instruction to the
interpreter, which runs each recorded call in a tracer (`src/cpu/record.c`): one instruction at a
time, reading the memory each one accesses before and after it runs. The record holds the
registers at entry, the memory the function read (the first time in each epoch), the memory it
wrote, and the calls it made with their registers. Calls run at full speed; after each one the
memory the function reads is recorded again, so what the callee changed is in the record.

`tools/recomp/replay.sh` replays each record in a child process: the game image is mapped where it
was, memory is rebuilt from the record, and the function runs in the interpreter. Its calls are not
run: each must match the recorded target and argument registers, and is answered with the
recorded registers and memory. The registers at the return and every byte written must match.

Replaying the original function checks the record; generated code is checked the same way.
A record is flagged when another thread changed memory the function read during the call; such
calls (locks, job queues) cannot be replayed alone. The game's code must only touch guest memory
for a record to be replayable: the variables the game imports (the stack canary) live in guest
memory for that reason.

First results (2026-10-10): 8 functions, 50 calls each, recorded during the Hunter's Dream
cutscene with no visible slowdown. The 7 ordinary functions replay exactly on all 350 calls. The
job loop (`0x21b8710`) matches on 31 of 50; 18 of the others had other threads' writes.


## What the image contains

162,959 functions with unwind information (median 80 bytes), about 10 million instructions;
241,821 functions in all with those found through calls, jumps and code pointers (about 11.3
million instructions). No
C++ RTTI. About 500 source paths in assertion and log messages name the libraries
(`tools/recomp/label.py`, 2026-10-24, all 241,821 functions):

| Library | Functions | Code | Known by |
|---|---:|---:|---|
| Game (`SPRJ\Source\SPRJ\Source\Game`, `Sys`) | ~37,100 | 7.8 MiB | source paths |
| Dantelion2 (FromSoftware's core library) | ~35,800 | 7.4 MiB | source paths |
| Havok 2014.1 (physics, AI, cloth) | ~28,500 | 9.7 MiB | source paths, names |
| Scaleform GFx (menus, HUD) | ~20,700 | 4.2 MiB | messages |
| FD4 (FromSoftware's engine) | ~16,400 | 4.1 MiB | source paths, names |
| FMOD Ex 4.44.50 | ~7,200 | 1.4 MiB | source paths |
| Lua (LuaPlus) | ~3,000 | 1.0 MiB | source paths |
| YEBIS (post-processing) | ~2,400 | 0.8 MiB | source paths |
| zlib | 44 | 34 KiB | messages |
| Other paths | 454 | 0.1 MiB | source paths of no library above |
| Not labelled yet | ~90,300 | 7.1 MiB | |

The labels start from the paths and messages (5,742 functions) and spread from there: to the
functions between two of the same library (109,686), then to the other entries of a vtable
(25,116) and to functions whose callers or callees all belong to one library (11,018). Labelled
functions hold 83.8% of the code. Hiding a tenth of the labels and running the last step again
gives back 7,158 of 11,497, 96.4% of them the same: the steps agree with each other, but the
boundaries between game, FD4 and Dantelion2 stay approximate. The functions left are small
(median 18 bytes) and most have no direct caller (84,456).

## Where the CPU time goes

Profiles in the Hunter's Dream (2026-10-10): the game's main thread is about 95% busy in a
cutscene and spread thin in play (933 functions ran in 20 s; the busiest 145 take half of the
time; the job loop is the largest at 7.3%). No small set of functions dominates, so the CPU side
gets faster by recompiling all of them, which R2 did; R3 makes that code cheaper.

CPU time (2026-10-24, M3 Pro, standing in the Hunter's Dream at about 730 draws a frame,
`ps -o cputime=` over 30 s): the translator alone takes 418–429% of one core, the full library
411–426% (three runs, mean 420%), the same within the runs' spread. Two changes got it there. The
lookup of recompiled functions became one flat table (529–539% before, 469–485% after): its 16,384
chained buckets held about 15 functions each, and every indirect call walked them. Then the
instructions the generated C still left to the interpreter got C of their own: a counter in the
runtime's single-step path found 11.8 million of them a second in this scene, 98% of them about
20 mnemonics (`vpshufb`, `tzcnt`, `idiv`, `mul`, `vmovlps`, `vextractf128`, `lzcnt`, `bextr`,
`rdtsc`, `andn`, `rol`, the `vpunpck` family, `blsr`, `div`, `vcvtsd2ss`, `vptest`, `vroundss`),
on the FMOD mixer, the render thread and an unnamed thread most. In C they leave about 36,000 a
second (string moves and `mfence`). In a `sample`, the game's code costs the same recompiled as
translated (about 3.1 cores); what is left in the runtime is `rc_call`, the path of indirect calls
and calls into the translator (a second: 4.3 million calls reach recompiled functions through the
runtime, 990,000 go into the translator, 1.75 million straight to host imports).

In a heavier scene the library still took more than the translator: at the 1st Floor Sickroom
lamp (about 1,283 draws a frame, 39 FPS) 409% against 404% of one core. A `sample` there put the
difference on the FMOD mixer thread, 90% of a core with the library against 45% translated, nearly
all of it in two of FMOD's mixing functions (about 600 vector register operands each). The
generated C kept vector registers in the CPU state, and the barrier after each vector load and
before each vector store (x86 ordering, as the translator has) made clang store them there and load
them again around every one: the translator keeps them in NEON registers. With the vector
registers a function names in locals, those two functions have half the loads and stores (2,891 to
1,450 and 3,001 to 1,744) and 34% and 21% fewer instructions, and the library is cheaper than the
translator in both places (2026-10-24, three alternating runs each):

| | 1st Floor Sickroom (1,283 draws) | Hunter's Dream (735 draws) |
|---|---:|---:|
| Translator alone | 399, 407, 406% (404%), 39.2 FPS | 439, 430, 431% (433%), 59.8 FPS |
| Library before (main `a8b96c3`) | 402, 410, 415% (409%), 39.3 FPS | 430, 423, 431% (428%), 60.3 FPS |
| Library, vector registers in locals | 379, 383, 382% (381%), 38.8 FPS | 402, 401, 402% (402%), 58.9 FPS |

The frame rate is the same within about 1%; per frame, CPU time is 5% (Sickroom) and 6% (Dream)
lower than the translator's. The library's code grew 8% (369 to 399 MB), from the copies around
calls. Recorded calls of the 60 busiest functions in that scene replay the same as the original
code (2,944 of 3,018; the 74 others fail with the original too: one function reads `rdtsc`, and
the job loop sees other threads' writes).

`rc_call` and `rc_tail`, the runtime's path for the calls the library does not make directly
(the runtime counts about 11.5 million
calls of recompiled functions a second at the Sickroom lamp, nearly all through them), took about 7% of a core in a `sample` there:
on every call a thread-local read for the `BB_RECOMP_THREADS` filter, `BB_RECOMP_NODIRECT`'s flag,
and the call counter in thread-local storage (`_tlv_get_addr`). Now the counter lives in `BbCpu`,
the filter and `BB_RECOMP_NODIRECT` are checked only by the versions of `call` and `tail` the
library gets when one of them is set, and the slow paths are out of line: `rc_call` is a leaf with
no stack frame (the lookup, the push, the count, a jump to the function). The library did not
change. Alternating runs in one session (2026-10-24):

| | 1st Floor Sickroom (1,283 draws) | Hunter's Dream (735 draws, one run each) |
|---|---:|---:|
| Translator alone | 396, 398, 400% (398%), 39.5 FPS | 409%, 60.0 FPS |
| Library, previous `rc_call` (`2a5284b`) | 378, 376, 377% (377%), 39.0 FPS | 406%, 60.8 FPS |
| Library, `rc_call` without TLS | 372, 375, 372% (373%), 39.1 FPS | 387%, 60.0 FPS |

At the Sickroom every run of the new `rc_call` (three more: 374, 373, 375%) is below every run of
the previous one: about 4 points, 1% of the game's CPU time. The Dream runs only check that it did
not get worse; single runs there vary more than that.

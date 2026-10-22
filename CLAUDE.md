# bloodborne-recomp

A static recompiler for Bloodborne (PS4, CUSA03173 1.09) and bbcpu, the CPU layer it stands on.
It reads the player's own `eboot.bin` and writes the game's functions as C, compiled into a native
library the game loads (`BB_RECOMP_LIB`, in bloodborne-runtime). Plan, milestones and results:
`docs/RECOMPILATION.md` and the wiki's Recompilation-status page.

## Layout

- `src/cpu/`: bbcpu. Decoder (Zydis), interpreter (`interp*.c`), arm64 translator
  (`jit_arm64.c`), record/replay tracer (`record.c`), natives (`native.c`), the recompiled-code
  runtime (`recomp.c`: loading, `rc_call`/`rc_tail`/`rc_bail`, frames with their own return
  addresses, the import fast path). Hosts link `out/libbbcpu.a` and provide `runtime_low_map`.
- `src/recomp/rc.h`: what generated code includes (API version `BB_RECOMP_API_VERSION`; bump it
  when RcApi or RcFunction change).
- `tools/recomp/`: `scan.c` (function table: unwind tables + calls, jumps, lea, relocation
  pointers), `bbrecomp.c` (the generator), `recomp.sh`, `replay.c`/`replay.sh`, `fuzz.sh`,
  `ps4elf.py`, `pointers.py`, `imports.py`, `profile.py`, `label.py`, `inspect.sh` (Ghidra).
- `tests/`: fuzzers and CPU tests. `out/` and `deps/` are not tracked.

## Build and check

```sh
./build.sh                                    # Zydis into deps/, out/libbbcpu.a, scan, bbrecomp
tools/cpu/test_jit.sh                         # CPU tests (CI runs them with the arm64 encodings)
GAME=~/Downloads/PS4/Bloodborne/CUSA03173     # the user's dump
python3 tools/recomp/ps4elf.py "$GAME/eboot.bin" out/eboot.elf
python3 tools/recomp/pointers.py "$GAME/eboot.bin" out/recomp/pointers.txt
out/recomp/scan out/eboot.elf out/recomp out/recomp/pointers.txt
python3 tools/recomp/imports.py "$GAME/eboot.bin" out/recomp
tools/recomp/recomp.sh --file LIST            # LIST: offsets; all: tail -n +2 out/recomp/functions.tsv | cut -f1
tools/recomp/fuzz.sh                          # must stay at 0 failed, 0 crashed
tools/recomp/replay.sh RECORD.rec --recomp out/recomp/librecomp.dylib
```

A change to the generator or the runtime is checked by: the fuzzer; replays of recorded calls
(records come from the game with `BB_RECORD`, see RECOMPILATION.md); and a game run through
bloodborne-runtime (the runtime's `tools/test_run.sh NAME BB_RECOMP_LIB=$PWD/recomp/out/recomp/librecomp.dylib`,
run from the runtime checkout).
The full build (241,031 functions) takes ~11 minutes and makes a ~400 MB library; only files whose
C changed are compiled again.

## Debugging recompiled code

- Crash markers in a game log: `STOP:`, `Host fault`, `Guest fault`, or the run failing to reach
  the level. `== Stall during rendering` is **not** one (healthy runs print it).
- A GPU assertion while loading (`Unimplemented PM4 type 0`) was two game threads writing one
  command buffer (fixed in bloodborne-runtime: `runtime_cmdbuf.c`, bbcpu guards); recompiled
  functions call each other directly and are not guarded, so if it comes back with the library
  only, look there first. `BB_CB_PROBE=1` names the threads.
- Bisect with `BB_RECOMP_ONLY=@FILE` (keep only these) or `BB_RECOMP_OFF=@FILE`; also
  `BB_RECOMP_THREADS` / `BB_RECOMP_NOT_THREADS` (by thread name) and `BB_RECOMP_NODIRECT=1`.
  `BB_RECOMP_TARGETS=1` prints the busiest calls into the translator every 10 s.
- Lessons so far: import stubs and `setjmp` callers stay with the translator; every runtime call
  into the translator has its own return address; jump tables are decoded from their `cmp`/`ja`
  bound. `BB_REPLAY_WATCH=ADDRESS` traces one byte in a replay.
- REA (an MCP server with Ghidra, on the user's machine) answers "who calls this", "decompile
  this" and "what references this address" about the game's code; its output is game code and
  never goes into a repository.
- Fault diagnostics: `BB_JIT_FAULT_MAP=1` reports the exact translated guest instruction;
  `BB_JIT_WATCH` records selected field accesses without function-entry callbacks. Usage is in
  `docs/ARM64_NATIVE.md`. Their game-derived output stays outside Git and the wiki.
- Speed: the frame rate is bound by rendering; compare CPU time (`ps -o cputime=` over 30 s in
  the level) between alternating runs with and without the library.

## Where it stands and what is next

- R2 done: all 241,031 functions generated (98.8% of instructions in C), 240,995 run in the game.
  Same frame rate as the translator but **20–30% more CPU time** (5.2–5.5 cores against 4.3 in the
  Hunter's Dream, `ps -o cputime=` over 30 s). R3 is making it cheaper. Candidates, measure each:
  the copy of the 16 registers to and from `BbCpu` around every call; vector registers kept in
  `BbCpu` instead of locals; acquire/release on every stack access; code layout of the 400 MB
  library (hot functions together, cold code at -Os); the remaining calls into the translator
  (wide-character libc functions; check the PS4's `wchar_t` size first). A `sample` of a run
  with the full library comes out nearly empty (its symbols are too large): profile with the
  49k-function library (`out/recomp/ran.txt`) instead.
- Data from earlier sessions, in `out/recomp/` of the user's clone (game-derived, never commit):
  `records40/` (40 recorded calls of each of the 933 busiest functions: 911 match, the rest also
  fail with the original), `hot.txt` (those 933), `ran.txt` (the 49,095 a 3-minute route ran),
  `all.txt`, `jitmap.txt`. Regenerate `functions.tsv` and the rest with the commands above.
- R5 (the native renderer) is planned and worked on in bloodborne-runtime
  (`docs/NATIVE_RENDERER.md`). Its part here: calls of hooked functions (bbcpu guards, natives)
  from recompiled code must go through the runtime, not directly to the recompiled callee.
- Next after R3: more routes (other areas, bosses), names for known library code (zlib, Lua) and
  the game's imports in the generated C (R4), recompiling at install time from the launcher.

## Working rules (all four repositories)

- **The project is four repositories**, nested as submodules:
  [bloodborne-mac](https://github.com/bloodborne-native/bloodborne-mac) (the app) ⊃ `runtime/` =
  [bloodborne-runtime](https://github.com/bloodborne-native/bloodborne-runtime) ⊃ `recomp/` =
  [bloodborne-recomp](https://github.com/bloodborne-native/bloodborne-recomp); the app also has
  `launcher/` = [bloodborne-launcher](https://github.com/bloodborne-native/bloodborne-launcher).
  Commit in the repository that owns the file, push it, then bump the submodule pointer in the
  parent and push that too. Submodules are checked out at a commit: run `git checkout main` (and
  `git pull`) there before editing.
- **Before any push, check `git remote get-url origin`.** Push to `main`; ask before
  force-pushing anything that existed before this session.
- **Commits:** a short subject, a body that says what changed and why (numbers when measured),
  and the trailer the session asks for.
- **Credits:** built on [bbport](https://github.com/deadinside28/bloodborne_pc) by deadinside28
  and on [shadPS4](https://github.com/shadps4-emu/shadPS4); keep them credited. Code taken from
  another project or fork is credited in the same commit: CREDITS.md, the wiki's credits page,
  the commit message and a short code comment.
- **No game code, ever:** no decompiled or generated game code, no game files, no records of game
  memory in any repository or wiki.
- **Testing the game:** `tools/test_run.sh` (a copy of the save; the real one is never written),
  in English, one game at a time (`pgrep -x bb-probe` first). Test windows open on the user's
  screen: say so before long batches, and stop when asked.
- **Never capture the user's screen.** Pictures come from the game: `tools/grab.sh`.
- **Shell:** no `rm -rf` on variable paths; no `xargs ... sh -c '...'`.
- **Docs:** plain, precise English; numbers measured, not guessed. Docs say what is true now;
  investigation logs do not go in docs (conclusions do). The wiki mirrors the docs; update both.

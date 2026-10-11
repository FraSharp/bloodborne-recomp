# Documentation

[← Back to the project](../README.md)

- [Recompilation](RECOMPILATION.md): the plan, the milestones, how the recompiler works, its tools,
  record and replay, what the game's executable contains, and the results so far.
- [Native arm64](ARM64_NATIVE.md): bbcpu, the x86-64 → arm64 translator and interpreter the
  recompiled code stands on.
- [Credits and licenses](CREDITS.md)
- Progress: the wiki's [Recompilation status](https://github.com/bloodborne-native/bloodborne-recomp/wiki/Recompilation-status).

The game side (loading the recompiled library, recording calls in the game) is
[bloodborne-runtime](https://github.com/bloodborne-native/bloodborne-runtime); the macOS app is
[bloodborne-mac](https://github.com/bloodborne-native/bloodborne-mac).

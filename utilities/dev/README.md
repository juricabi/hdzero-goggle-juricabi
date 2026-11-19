# Developer scripts (not part of the firmware)

The scripts used to build, run and test `ripples-all` on a Linux host (WSL Ubuntu 24.04, as root); the
same as on `all`. Paths default to `/root/hdz/...` and can be changed by the environment variables named in
each script. Status of this branch: [docs/status.md](../../docs/status.md).

| script | what |
|---|---|
| `build_all.sh <worktree> [goggle\|goggle2\|boxpro ...]` | the flashable images (`HDZERO_GOGGLE-…-<commit>.bin` + `.sha256`) into `$OUT` (default `/root/hdz/out`), with the toolchain in `$TOOLCHAIN` (default `/root/hdz/toolchain`: Bootlin `armv7-eabihf--musl--stable-2018.02-2`, the one `setup.sh` fetches); the same CMake build as ripples' own (`setup.sh`, `build_*`), build directories outside the worktree; refuses a worktree on a Windows drive (jffs2 needs the file modes) |
| `emu_build.sh <goggle\|goggle2\|boxpro>` | the SDL emulator build of this worktree (host compiler) into `$EMU_OUT/build_<target>` |
| `emu_run.sh <build dir> <setting.ini\|-> <key script> <shot prefix>` | runs the emulator **sandboxed**: its own Xvfb, a private mount namespace with tmpfs `/mnt` and `/tmp`, uid 65534 without capabilities, every system command replaced by a logging stub; keys from a script, screenshots and logs into `$EMU_OUT/shots` and `$EMU_OUT/logs`. ripples' emulator already runs no shell command (`system_exec()` is a no-op there); the sandbox stays, as on `all`. Needs `Xvfb`, `xdotool`, ImageMagick `import`. |
| `keys/*.txt` | key scripts for `emu_run.sh` (give their absolute path: it reads them after a `cd`): `k <key> [n]` tap (`s` down, `w` up, `d` click, `a` right button), `l <key>` hold 0.9 s, `w <s>` wait, `s <name>` screenshot, `c <name>` copy setting.ini; other lines are ignored (the first line says which goggle the script is for). ripples' menu: `k w 2` from the top = Module Bay (the entry before Go Sleep!) on both goggles. `check_start.txt` is the start check of docs/status.md; `wifi_stream_audio.txt` opens WiFi Module > System and clicks Stream Audio without applying (any target; `k s 10` from the top). `audio_page.txt` opens the Audio page (`k s 7`), selects every row in turn with a screenshot and leaves it without a change; `audio_change.txt` (Goggle 2, BoxPro) changes Mic, Record Headphone Mix, Audio Source and Live Volume one step each and runs the DVR test, a copy of setting.ini after each. |
| `run_ripples_tests.sh` | ripples' own tests (`test/*`) on a copy of the worktree, as an unprivileged uid in its own pid and mount namespace with a private `/tmp`; logs in `$TEST_OUT/ripples_tests` |
| `run_live.sh all\|<t_script> [args]` | the live-stream audio suite (`utilities/live_audio_test`, docs/live-audio.md section 3) as root in private mount, IPC, network and pid namespaces with a tmpfs `/tmp`: its fixed pid files, SysV keys, port 18554 and `pkill -x rtspLive` reach nothing else; `all` builds and runs it (`run_all.sh`), a single `t_*.sh` uses what the last `all` built; `RTSPLIVE=arm ARM_RTSPLIVE=<build>/rtspLive` runs the cross-built rtspLive under qemu-arm (no host live555 here: the native rtspLive tests need `LIVE555`); logs in `$TEST_OUT/live` |
| `run_probe.sh run\|mutants` | the probe kit's tests as an unprivileged uid in its own pid namespace (`utilities/probe/test/run_tests.sh` and `mutants.sh` refuse to run as root); `SHELLS`, `ONLY`, `QUICK`, `ROOTFS` are passed on |

The other suites run directly: `make -C utilities/module_bay_test test`, `utilities/module_bay_test/mutants.sh`,
`python3 utilities/module_bay_test/check_strings.py`, the C tests of the HDMI audio code
(`sh utilities/probe/test/it66121_seq_test.sh`, `it66021_edid_test.sh`, `pcmbridge_test.sh`, `hdmi_boot_test.sh`;
the first two compare with ripples-base 4fbb865 from git) and their mutants (`utilities/probe/test/c_mutants.sh`),
the audio mixer's host test, its mutants and the Audio page's strings (`python3 utilities/audio_mixer/test_mixer.py`,
`mutants.py` (in parallel, `--jobs`), `check_strings.py`; docs/audio.md), and ripples' own tests (`make -C test/<area> run`,
see docs/status.md for the list and how they are run here).

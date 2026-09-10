# Build and Flash

## Toolchain

**ESP-IDF v5.5.1**, target `esp32s3`. The BSP requires ≥5.5
([ADR 0005](adr/0005-consume-waveshare-bsp.md)); CI pins the same version so local and CI
builds match.

The local install on the development machine is at `~/esp/v5.5.1/esp-idf`.

## Build

**On Windows, use PowerShell.** ESP-IDF explicitly refuses to run under Git Bash / MSYS
(*"MSys/Mingw is not supported"*), so `export.sh` cannot be used there even though the file
exists.

```powershell
# The IDE-installed environment is Python 3.11. Setting this explicitly avoids export.ps1
# deriving the wrong venv name from whichever `python3` happens to be on PATH.
$env:IDF_PYTHON_ENV_PATH = "C:\Users\<user>\.espressif\python_env\idf5.5_py3.11_env"
. "C:\Users\<user>\esp\v5.5.1\esp-idf\export.ps1"

idf.py -C firmware set-target esp32s3
idf.py -C firmware build
```

Or use the ESP-IDF VSCode extension, which handles the environment itself — set the target to
`esp32s3` and build normally.

On Linux and macOS the usual `. $IDF_PATH/export.sh` works with no special handling. CI builds
in Espressif's Docker image, so none of the above applies there.

The first build downloads managed components (the Waveshare BSP, LVGL, `esp_lvgl_port`,
LittleFS) into `firmware/managed_components/`. That directory is generated and git-ignored.

## Flash and monitor

```bash
idf.py -C firmware -p COM<N> flash monitor
```

Exit the monitor with `Ctrl+]`.

If the board is not detected, hold **BOOT** while pressing **RESET** to force download mode.

## Flashing a CI artifact

CI produces a single merged image containing bootloader, partition table, application and
filesystem — see [ci-release.md](ci-release.md).

```bash
esptool.py --chip esp32s3 -p COM<N> write_flash 0x0 ai-gauge-merged.bin
```

Or flash a tagged release straight from a Chromium browser via the ESP Web Tools page, with
no toolchain installed at all.

## Filesystem image

Default gauge configs in `firmware/assets/` are built into a LittleFS image and flashed to the
`storage` partition. To update just the filesystem without reflashing the application:

```bash
idf.py -C firmware storage-flash
```

At runtime, configs are more conveniently replaced over HTTP — see
[networking.md](networking.md). That path re-renders the face live, without a reboot.

## Useful commands

| Command | Purpose |
|---|---|
| `idf.py -C firmware menuconfig` | Change configuration interactively |
| `idf.py -C firmware size-components` | Where the flash is going |
| `idf.py -C firmware app-flash monitor` | Reflash only the app — much faster iteration |
| `idf.py -C firmware fullclean` | Full rebuild, including managed components |
| `idf.py -C firmware save-defconfig` | Write current config back to `sdkconfig.defaults` |

## Configuration

`firmware/sdkconfig.defaults` is committed and is the source of truth. `sdkconfig` itself is
generated and git-ignored.

**Change configuration by editing `sdkconfig.defaults`, or by running `menuconfig` and then
`save-defconfig`.** Editing `sdkconfig` directly means the change is lost on the next clean
build and never reaches CI.

Settings that carry a performance justification are commented in the file. Do not change those
without re-measuring — see [performance.md](performance.md).

## Host-side tests

`gauge_config` has no ESP-IDF or LVGL dependency, so its parser tests build and run on the
development machine with no board attached. CI runs them on every push.

```bash
cmake -S tools/host-tests -B build/host-tests
cmake --build build/host-tests
ctest --test-dir build/host-tests --output-on-failure
```

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| Component download fails | Network or registry issue; retry, or check `firmware/main/idf_component.yml` |
| `MSys/Mingw is not supported` | Running `export.sh` from Git Bash on Windows — use PowerShell and `export.ps1` |
| `Python virtual environment ... not found` | `export.ps1` derived the venv name from the wrong `python3`; set `IDF_PYTHON_ENV_PATH` as shown above |
| Flash mode reads `dio` despite `FLASHMODE_QIO=y` | Expected — the bootloader starts in DIO and upgrades itself to quad mode during init |
| `Target esp32s3 not supported` | BSP needs IDF ≥5.5 — check `idf.py --version` |
| Blank display, no errors | PSRAM config — the octal PSRAM settings in `sdkconfig.defaults` are required |
| Boot loop after flashing | Partition table changed without erasing; run `idf.py -C firmware erase-flash` first |
| Poor frame rate | See [performance.md](performance.md); confirm `CONFIG_COMPILER_OPTIMIZATION_PERF` and the dual draw units are set |

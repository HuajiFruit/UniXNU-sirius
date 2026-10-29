# Device agent guide

This repository owns the sirius board: its hardware description, the selection of shared
components, the payload policy and the porting record. It does **not** own the GIC/counter/PSCI
facts (that is [UniXNU-sdm710-common](https://github.com/57UU/UniXNU-sdm710-common)), the XNU
source or the OS profile (those are UniXNU), or the U-Boot firmware (that is sirius-distro).

## Read first

Read [Where the boot stands](#where-the-boot-stands) first, then `README.md`,
`docs/HANDOFF.zh-CN.md` and these UniXNU pages before changing the port:

| Task | Required reading |
| --- | --- |
| Every port | [Porting](https://unixnu.com/porting/), [device repository](https://unixnu.com/porting/device-repository), [component catalog](https://unixnu.com/reference/components) |
| Hardware | [Device tree](https://unixnu.com/porting/device-tree), the sdm710 SoC `README.md`, `soc.toml` and `dts/sdm710.dtsi` |
| Kernel | [Source and patches](https://unixnu.com/xnu/source-and-patches), [patch reference](https://unixnu.com/xnu/patch-reference), the selected OS profile |
| Images | [Prelink](https://unixnu.com/concepts/prelink), [payload](https://unixnu.com/concepts/payload), [boot chain](https://unixnu.com/concepts/boot-chain) |
| Record | [Porting records](https://unixnu.com/porting/records) — `porting.toml` is machine-checked |

## Where the boot stands

Kernel `67b65eb` (commit `fb84613`), payload `1611d2a`; seconds after `fastboot reboot`. The
evidence is in `docs/evidence/` (`sirius-touch-hid-20261029-combined.log`,
`sirius-gpu-20261029-phase1.log`, `sirius-spawn-20261028-ephemeral.log`); `porting.toml` has the
rest.

| Stage | State |
| --- | --- |
| U-Boot → UniXNU.efi → DarwinBoot → XNU | works; DarwinBoot gives XNU a 1 GiB window (`owned_memory_size`) |
| CPU | A55s at 1708 MHz, A75s requested at 2208 MHz (`sirius-cpufreq=1`) |
| eMMC | 50 MHz, 8-bit HS SDR, 256-block ADMA2, about 30 MiB/s |
| Root, launchd, Data | APFS OS volume + SystemOS cryptex; 8 GiB persistent Data volume on eMMC (`disk3s1`) |
| Data migration | skipped: `/chosen/ephemeral-storage = 1` (`[adt.chosen.u32]`); its plugins waited on missing services for over 36 minutes. The staged system apps are not installed while it is set |
| UI | iOS reaches the Setup assistant ("hello", status bar, clock) at about 220–360 s, CPU-rendered: the GOP framebuffer is copied by the CPU (about 9–10 ms a frame, write-combined scanout, cached source) |
| Touch | **works**: FTS521 on QUP1 SE1 (`package/sirius_touch.c`, LDO6 through RPMh), published to iOS by `package/SiriusHidProbe.cpp` as ShengHidInput's combined keyboard+touch device; the in-kernel IOHIDEventDriver takes it (`dk=0`, `sheng-touch-hid=1`). Setup reacts to touch |
| GPU | Adreno 616 phase 1 done (`package/sirius_gpu.c`): CX GDSC, GPU bus clocks, GMU clock at 200 MHz, GMU and GPU SMMU registers readable. No firmware loaded yet, no Metal device |
| Known issues | the wallpaper poster extensions crash-loop (Kaleidoscope sig 11, Aegir sig 6); the USB console often stops after a few minutes while the phone keeps running; user stackshots are refused (`validation_disables=0x10`) because a stackshot panicked on this port |

Next, in order:

1. **GPU phase 2:** load `a630_gmu.bin`, boot the GMU and get HFI v1 replies (legacy A630-family
   path). The GMU reads its HFI queues through the GPU SMMU (stream 5), so this needs a context
   for that stream on the GPU SMMU at `0x05040000` (never the apps SMMU). Then GX, ZAP, SQE and a
   fence (phase 3), then the Adreno6xx fork with an A616 backend and AdrenoMetal for a Metal
   device. Hardware values and milestones are in the `porting.toml` gpu area.
2. **Touch polish:** buttons (power and volume, through `native_buttons_set_sink`), and why the
   DriverKit HID server is never launched (`dk=0` keeps every dext off).
3. **Data migration:** find what its plugins wait on, so `ephemeral-storage` can go and the
   system apps install.
4. Later: DDR52/HS200 eMMC; more than a 1 GiB window below 4 GiB; the other drivers of
   57UU/linux-sirius (battery, RTC, haptics, backlight, Wi-Fi, audio); a clean upstream PR to 57UU.

## Work

Everything about this port is already implemented once in the `legacy` branch. Prefer reading
that history over re-deriving a value from scratch.

1. A shared SoC value (GIC base, counter, PSCI, timer INTID) goes to the SoC repository, not here.
2. A board value (memory, CPU selection, payload policy, Apple device-tree deviations) goes in
   `board.toml` or `dts/sirius.dts`.
3. A change to XNU itself belongs in a series: the ARM64/OS series of UniXNU when it is general,
   `patches/xnu/series` of this repository when it is board specific.
4. A board fork of a component stays under `drivers/<ID>` and is listed in `board.toml`
   `[components] include` by its canonical ID.
5. Update the affected `porting.toml` area in the same change: findings or evidence, with the
   image hashes and the checkpoint that was observed. The check is per push against the previous
   push: a file under an area's `paths` (`board.toml`, `configs/**` and `.github/**` are the
   configuration area; `package/**`, `patches/xnu/**`, `docs/evidence/**` and the capture script
   are diagnostics; the eMMC sources are storage) needs that area's findings or evidence in the
   same push, or the Device documentation workflow fails.
6. Evidence revisions are full 40-character commit hashes; take them from `git rev-parse`, never
   write one from memory.

## Checks

Run from a UniXNU checkout:

```sh
./unixnu defconfig sirius_defconfig -D ../UniXNU-sirius
./unixnu fetch
./unixnu status --json
./unixnu tree
python3 scripts/porting-record.py check --device ../UniXNU-sirius
python3 scripts/porting-record.py check --device ../UniXNU-sirius --base <previous pushed commit>
```

The Device documentation workflow runs the record check on every push, with `--base`. Run the
`--base` form before pushing.

## Hardware work

There is one bootable target and it is a phone: every experiment is a flash and a reboot.

- Kernel: `macOS + Xcode only`, or the `kernel` workflow of this repository (a push to `main`
  that touches `configs/`, `board.toml`, `scripts/`, `package/`, `patches/` or `drivers/` starts
  it; about 15 minutes). The bootstrap pass of its log always lists undefined board symbols;
  only the final link's errors matter.
- Payload: Linux is enough; `make-uboot-payload.py` finishes the volume. Pass
  `--trust-cache` (the original bundle, `7416ca4f…`) or AMFI kills launchd.
- Payload-only changes need no kernel build: `board.toml` `kernel_args`, `[payload]` and
  `[payload.adt]`/`[adt.*]` (device-tree values) go into `payload.img` alone. Reuse the last
  kernel and cancel the kernel run such a push starts.
- Flash: `fastboot flash vendor <payload>` (from the phone's own fastboot, Volume-down + Power;
  U-Boot's fastboot hangs on vendor). Large images go as Android sparse images with
  `fastboot -S 512M`: a raw 9 GB image makes Windows fastboot fail with `bad_alloc`.
- Evidence: the USB console is the log. `scripts/capture-usb-console.py` reads it on Linux
  (`/dev/ttyACM*`) or Windows (the `SIRIUS01` COM port); `--timeline FILE` records when each line
  first arrives, `--idle-stop 20` ends a capture once the phone goes quiet (a hard panic freezes
  the kernel before the heartbeat mirrors its text), `--match 'NEMMC PANIC TEXT END'` ends it on a
  mirrored panic. Keep a capture under 5 minutes (`--duration 280`, counted from the start).
  Record the kernel and payload hashes with each observation; trimmed excerpts go to
  `docs/evidence/`.

Keep these in the command line; each encodes a hardware fact or a bring-up decision (see
`docs/HANDOFF.zh-CN.md` and `porting.toml`):

| Argument | Why |
| --- | --- |
| `armcpu-timer-intid=19` | the SDM670/710 virtual timer is PPI 3, not Apple's PPI 11 |
| `sheng-smp=1` | the native ArmCpu has to own every `/cpus` nub, or AppleARMCPU panics |
| `sirius_emmc_hs=3` | the eMMC clock ladder (50 MHz on this card); without it reads crawl at 400 kHz |
| `sirius-allow-unverified=1` | authorizes the SystemOS union mount; there is no owner root policy |
| `sheng-disable-watchdogd=1`, `sheng-lite=1` | watchdogd traps without a watchdog backend; lite drops daemons with nothing to do |

Changing the command line requires rebuilding the payload and re-flashing the vendor partition.

Do not repeat these; each one cost a hardware round:

- Never set `CONFIG_XNU_NATIVE_PAN=n` and never clear PSTATE.PAN by hand: the CPU sets PAN on
  every exception entry, so without the PAN build copyin/copyout loop forever, and with it a
  clear PAN at a context switch panics.
- Never write the SMMU's S2CR/SMR from the driver: it has hung the SoC.
- The eMMC driver's shared bounce buffer stays under the transport lock until the copy into or
  out of the request is done; releasing it early handed one request another's blocks.
- Every Data seed build formats a new volume with a new UUID.

## Environment on the bring-up PC

| What | Where |
| --- | --- |
| fastboot | `C:\Users\jimde\Desktop\Files\adb-platform-tools\fastboot.exe` (run flashes from PowerShell) |
| gh | `C:\Program Files\GitHub CLI\gh.exe`, logged in (WSL's gh is not) |
| WSL | Ubuntu-26.04, user `huajifruit`; `sudo` asks a password, so install packages with `wsl -u root` |
| UniXNU checkout | `~/Xnu4Sirius/UniXNU`, `sirius_defconfig` selected with `-D ../UniXNU-sirius` |
| Build clone | `~/Xnu4Sirius/UniXNU-sirius`; remotes `origin` (fork), `upstream` (57UU), `win` (the Windows checkout) |
| IPSW, trust bundle | `~/Xnu4Sirius/UniXNU/refer/iPad_Pro_HFR_17.7.11_21H461_Restore.ipsw`, `build/original-static-system-trust/original-static-system.bundle` |
| Root image | `~/Xnu4Sirius/UniXNU/out/sirius/ipados-17.7.11-21H461/sirius/userdata/userdata-root.img`, sparse `~/Xnu4Sirius/flash/userdata-root.simg` |
| Images, kernels | `~/Xnu4Sirius/flash/`, `~/Xnu4Sirius/kernel/` |
| Seed passphrase | `~/Xnu4Sirius/.data-seed-passphrase` (the fork's `ARTIFACT_PASSPHRASE` secret holds the same value) |
| U-Boot image | `~/Xnu4Sirius/UniXNU-sirius/uboot/boot-unixnu.img` (`bash uboot/build.sh`) |

On the phone now: `boot` = `boot-unixnu.img`, `userdata` = `data-window.simg` (the 8 GiB Data
volume), `vendor` = `tmp/payload-AD-combined.img` (kernel `67b65eb`, payload `1611d2a`).

- Run WSL commands from script files: `wsl.exe -- bash -c '…'` from Git Bash loses `$variables`.
  Set `MSYS_NO_PATHCONV=1` in Git Bash, and give fastboot `\\wsl.localhost\Ubuntu-26.04\…` paths
  from PowerShell (or copy the image to a Windows folder first).
- `gh run watch` drops on network errors; poll `gh run view <id> --json status,conclusion` instead.
- Kernel artifacts expire after 3 days, seed artifacts after 7.
- Downloaded kernels and payload images for flashing go to `tmp/` in this checkout (ignored), not the
  Desktop: `gh run download <id> -D tmp/kernel-<name>`, and copy each payload from WSL to
  `tmp/payload-<name>.img` before `fastboot flash vendor tmp\payload-<name>.img`.
- Captures go to `out/` (ignored). The heartbeat re-mirrors the whole kernel log buffer, so a raw
  280 s capture runs to tens of MB; read the `--timeline` file first and commit only an excerpt.

```sh
# kernel: a push to main starts it, or
gh workflow run kernel.yml -R HuajiFruit/UniXNU-sirius --ref main -f steps=kernel
gh run download <run id> -R HuajiFruit/UniXNU-sirius -n sirius-kernel-<n> -D <dir>

# payload, in WSL
cd ~/Xnu4Sirius/UniXNU-sirius && git pull --ff-only origin main
cd ~/Xnu4Sirius/UniXNU
W=out/sirius/ipados-17.7.11-21H461/sirius
./unixnu build payload --kernel <kernel.macho> -D ../UniXNU-sirius \
  --trust-cache build/original-static-system-trust/original-static-system.bundle \
  --ipsw refer/iPad_Pro_HFR_17.7.11_21H461_Restore.ipsw          # [--data-ramdisk <seed>]
python3 ../UniXNU-sirius/scripts/make-uboot-payload.py --payload $W/payload/payload.img \
  --unixnu-efi $W/package/Build/Out/AARCH64-DEBUG/UniXNU.efi \
  --darwinboot-efi $W/tree/build/DarwinBoot/DEBUG_CLANGPDB/AARCH64/DarwinBoot.efi \
  --marker --output ~/Xnu4Sirius/flash/payload-<name>.img

# flash and capture, from PowerShell in this checkout (phone in fastboot)
fastboot flash vendor \\wsl.localhost\Ubuntu-26.04\home\huajifruit\Xnu4Sirius\flash\payload-<name>.img
fastboot reboot
python scripts/capture-usb-console.py --follow --wait 120 --duration 280 --output out/<name>.log `
  --timeline out/<name>.timeline --match 'NEMMC PANIC TEXT END' --idle-stop 20
```

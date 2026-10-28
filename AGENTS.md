# Device agent guide

This repository owns the sirius board: its hardware description, the selection of shared
components, the payload policy and the porting record. It does **not** own the GIC/counter/PSCI
facts (that is [UniXNU-sdm710-common](https://github.com/57UU/UniXNU-sdm710-common)), the XNU
source or the OS profile (those are UniXNU), or the U-Boot firmware (that is sirius-distro).

## Read first

Read `docs/NEXT-AGENT.md` first: where the boot stands, what is half done, and what comes next.
Then `README.md`, `docs/HANDOFF.zh-CN.md` and these UniXNU pages before changing the port:

| Task | Required reading |
| --- | --- |
| Every port | [Porting](https://unixnu.com/porting/), [device repository](https://unixnu.com/porting/device-repository), [component catalog](https://unixnu.com/reference/components) |
| Hardware | [Device tree](https://unixnu.com/porting/device-tree), the sdm710 SoC `README.md`, `soc.toml` and `dts/sdm710.dtsi` |
| Kernel | [Source and patches](https://unixnu.com/xnu/source-and-patches), [patch reference](https://unixnu.com/xnu/patch-reference), the selected OS profile |
| Images | [Prelink](https://unixnu.com/concepts/prelink), [payload](https://unixnu.com/concepts/payload), [boot chain](https://unixnu.com/concepts/boot-chain) |
| Record | [Porting records](https://unixnu.com/porting/records) — `porting.toml` is machine-checked |

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

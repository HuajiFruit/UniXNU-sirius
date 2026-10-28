# Next agent: where sirius stands and what to do

Written when the persistent Data volume was verified on hardware. Every claim below has evidence
in `porting.toml` and `docs/evidence/`. Read `AGENTS.md` for the rules (record check, payload-only
changes, arguments to keep, traps).

## Where the boot stands

Measured on the phone with kernel `66d0319` and the 8 GiB persistent eMMC Data window (timeline
seconds after `fastboot reboot`, `docs/evidence/sirius-usb-console-20261028-pdata.log`):

| Stage | State |
| --- | --- |
| U-Boot → UniXNU.efi → DarwinBoot → XNU | works; DarwinBoot gives XNU a 1 GiB window (`owned_memory_size`) |
| eMMC | 50 MHz, 8-bit HS SDR, 256-block ADMA2 runs, about 30 MiB/s; eMMC driver up at about 44–45 s |
| Root | APFS mounts the OS volume; SystemOS cryptex union-mounted over `/` |
| launchd | runs (PAN build); dyld maps the shared cache after the 4 KiB hash cache verifies it (about 100 s at boot) |
| Data | **8 GiB persistent eMMC Data volume** at LBA 35627008 behind root image, mounted at `/private/var` (`disk3s1`, OwnerData, UUID `0C28586B-...`) |
| Boot tasks | `mount -P 1` passes with `/chosen/boot-manifest-hash`; all tasks run; "Early boot complete" at 159.6 s |
| After that | **no panic up to 280 s; zero ENOSPC lines**; containermanagerd, launchd, analyticsd create containers without running out of disk; APFS transactions commit through xid 85; SpringBoard (pid 118) and liveactivitiesd (pid 128) crash for want of a display/framebuffer driver |
| Display / touch | **next blocker**: no display driver, screen shows only boot panel, SpringBoard crashes without framebuffer |

What got it here, all on `main` of the fork (not in 57UU's repository):

| Commit | Change |
| --- | --- |
| `4256f82`, `b1a11c4` | `CONFIG_XNU_NATIVE_PAN=y` and the driver no longer clears PAN: the init copyout fault loop was PAN, not a pmap bug |
| `24f6094`, `86fe9ff` | SystemOS union mount (bring-up mode) and the native 4K signature hash cache |
| `1163990`, `33509e8`… | eMMC multi-block reads and a verified clock ladder; 400 kHz had been left on by `sirius_emmc_prepare()` |
| `c30a0a0` … `396fdab` | Data seed served by NativeEmmc (RamBlock needs an AES closure this board lacks), seed workflow, seed taken whatever its UUID |
| `09096e3` | `/chosen/boot-manifest-hash` (uke's bring-up value) for `mount -P 1` |
| `b9922c1` | the shared bounce buffer stays under the lock (fixed APFS data hash mismatches); watchdogd and idle daemons disabled |
| `2fc36a1` | U-Boot submodule repinned to a commit that exists upstream (57UU's pin `667807d` was never pushed and breaks every CI checkout) |
| `e73a8ec` … | **Persistent Data volume on eMMC**: write path with ADMA2, CMD13 wait and CMD6 cache flush; double write fences (window + userdata minus first MiB); 8 GiB APFS container behind root image (LBA 35627008); fastboot sparse image generator (`make-data-window-image.py`) with DONT_CARE over root; verified on device with 0 ENOSPC |

## The current milestone & next blocker: Display

The Data space bottleneck is fully solved. The next blocker is **Display**:

iPadOS draws nothing and SpringBoard crashes (`Process[118] crashed: SpringBoard. Too many corpses being created.`).
U-Boot leaves a 1080x2240 splash framebuffer at `0x9c000000`; the `GopFramebuffer` component (UEFI GOP
to MobileFramebuffer) or `NativeLegacyDisplay` is the route the other ports took (asteroids, tb128fu).

1. **Display**:
   - Check how asteroids and tb128fu wire `GopFramebuffer` or `NativeLegacyDisplay`.
   - Framebuffer parameters: base `0x9c000000`, 1080x2240 (or 1080x2244 DSI), 32-bit ARGB/BGRA.
   - Publish `AppleCLCD` / `IOMobileFramebufferShim` so WindowServer / SpringBoard has a display device to attach to.
2. **Touch**: Synaptics S3706; the OnePlus 6T (fajita) port has a driver to adapt.
3. **Storage speed**: the card supports DDR52 and HS200 (`EXT_CSD type 0x57`); DDR52 needs no tuning.
4. **Memory**: read the real map from DarwinBoot to see if more than 1 GiB window can be allocated below 4 GiB.
5. **Upstream PR**: 57UU's repository still has broken CI and lacks PAN/eMMC/Data fixes. A clean PR would help upstream.
   save them the same rounds.
6. `CONFIG_XNU_OWNER_DATA_VOLUME_UUID` names the 256 MiB RAM seed (`E06A21C1…`); keep it in step
   with the seed in use (the driver logs a mismatch and mounts anyway).

## Environment on this PC

| What | Where |
| --- | --- |
| fastboot | `C:\Users\jimde\Desktop\Files\adb-platform-tools\fastboot.exe` (run flashes from PowerShell) |
| gh | `C:\Program Files\GitHub CLI\gh.exe`, logged in (WSL's gh is not) |
| WSL | Ubuntu-26.04, user `huajifruit`; `sudo` asks a password, so install packages with `wsl -u root` |
| UniXNU checkout | `~/Xnu4Sirius/UniXNU`, `sirius_defconfig` selected with `-D ../UniXNU-sirius` |
| Build clone | `~/Xnu4Sirius/UniXNU-sirius`; remotes `origin` (fork), `upstream` (57UU), `win` (this Windows checkout) |
| IPSW, trust bundle | `~/Xnu4Sirius/UniXNU/refer/iPad_Pro_HFR_17.7.11_21H461_Restore.ipsw`, `build/original-static-system-trust/original-static-system.bundle` |
| Root image | `~/Xnu4Sirius/UniXNU/out/sirius/ipados-17.7.11-21H461/sirius/userdata/userdata-root.img`, sparse `~/Xnu4Sirius/flash/userdata-root.simg` |
| Images, kernels | `~/Xnu4Sirius/flash/`, `~/Xnu4Sirius/kernel/` |
| Seed passphrase | `~/Xnu4Sirius/.data-seed-passphrase` (the fork's `ARTIFACT_PASSPHRASE` secret holds the same value) |
| U-Boot image | `~/Xnu4Sirius/UniXNU-sirius/uboot/boot-unixnu.img` (`bash uboot/build.sh`) |

On the phone now: `boot` = `boot-unixnu.img` (boots UniXNU when the vendor volume carries the
marker), `vendor` = the kernel `396fdab` payload with the 256 MiB RAM seed, `userdata` = the root
image followed by leftovers of the old Linux rootfs. Volume-down + Power enters fastboot at any time.

Practical notes:

- Run WSL commands from script files: `wsl.exe -- bash -c '…'` from Git Bash loses `$variables`.
  Set `MSYS_NO_PATHCONV=1` in Git Bash, and give fastboot `\\wsl.localhost\Ubuntu-26.04\…` paths
  from PowerShell.
- `gh run watch` drops on network errors; poll `gh run view <id> --json status,conclusion` instead.
- Kernel artifacts expire after 3 days, seed artifacts after 7.

## The loop

```sh
# kernel: a push to main starts it, or
gh workflow run kernel.yml -R HuajiFruit/UniXNU-sirius --ref main -f steps=kernel
gh run download <run id> -R HuajiFruit/UniXNU-sirius -n sirius-kernel-<n> -D <dir>

# payload, in WSL
cd ~/Xnu4Sirius/UniXNU-sirius && git pull --ff-only origin main
cd ~/Xnu4Sirius/UniXNU
W=out/sirius/ipados-17.7.11-21H461/sirius
./unixnu build payload --kernel <kernel.macho> \
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

Read `out/<name>.timeline` first: one line per distinct console line with its arrival time.

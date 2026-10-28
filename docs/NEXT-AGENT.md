# Next agent: where sirius stands and what to do

Written when the work stopped. `main` of the fork
[HuajiFruit/UniXNU-sirius](https://github.com/HuajiFruit/UniXNU-sirius) is at `2f29651`; every
claim below has evidence in `porting.toml` and `docs/evidence/`. Read `AGENTS.md` for the rules
(record check, payload-only changes, arguments to keep, traps).

## Where the boot stands

Measured on the phone with kernel `396fdab` and a 256 MiB RAM Data seed (timeline seconds after
`fastboot reboot`, `docs/evidence/sirius-usb-console-20261028-data256.log`):

| Stage | State |
| --- | --- |
| U-Boot → UniXNU.efi → DarwinBoot → XNU | works; DarwinBoot gives XNU a 1 GiB window (`owned_memory_size`) |
| eMMC | 50 MHz, 8-bit HS SDR, 256-block ADMA2 runs, about 30 MiB/s; eMMC driver up at about 51–58 s |
| Root | APFS mounts the OS volume; SystemOS cryptex union-mounted over `/` |
| launchd | runs (PAN build); dyld maps the shared cache after the 4 KiB hash cache verifies it (about 100 s at boot) |
| Data | RAM seed served by NativeEmmc, mounted at `/private/var` before launchd |
| Boot tasks | `mount -P 1` passes with `/chosen/boot-manifest-hash`; all tasks run; "Early boot complete" at about 170 s |
| After that | no panic up to 336 s; one daemon crash; **the Data volume fills about 60 s after early boot** |
| Display / touch | nothing: no display driver, the screen shows only the boot panel |

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

## The blocker: Data space, and the half-done fix

A first iPadOS boot writes far more than a RAM Data volume can hold (`containermanagerd` creates a
data container per app, plus launchd, analyticsd and APFS). The user chose a **persistent Data
volume on the eMMC**, in the free part of `userdata` behind the 8.74 GiB root image (about 41 GiB).

Branch **`wip/persistent-data`** (`e81b9e3`) holds the unfinished work. It is **not built, not
booted and not recorded in `porting.toml`**:

- `package/sirius_emmc.c`: writes (CMD24/CMD25 with Auto-CMD12 through the ADMA2 run, CMD13 until
  programmed, CMD6 FLUSH_CACHE when EXT_CSD CACHE_CTRL is on), fenced twice: the one window the
  driver opens (`sirius_emmc_set_write_window`, once) and userdata minus its first MiB.
- `package/NativeEmmc.cpp`: a read-write eMMC Data window starting at the first MiB behind the root
  image the SHNGROOT manifest records (userdata byte 9382658048, LBA 35627008), sized by the APFS
  container found there (NXSB superblock), published only with `sirius-data-write=1`; write
  requests through the queue (bounce under the lock both ways); a cache flush on synchronize; the
  Data hook takes the volume on whichever Data window exists, and the RAM seed is only published
  when there is no persistent window.
- `scripts/make-data-window-image.py`: turns the grown container into a fastboot sparse image for
  `userdata`: DONT_CARE over the root image, RAW for the container's nonzero blocks, FILL for its
  zero blocks. A `simg2img` round trip on a small fixture reproduced the container byte for byte.
- `board.toml`: `sirius-data-write=1`, `[payload] size_mib` back to 256; `data-seed.yml`: default
  container size 8192 MiB.

To finish it:

1. Review the branch diff (`git diff main wip/persistent-data`), merge it into `main`, and record it
   in the same push: configuration (the argument, the size, the workflow default), storage (write
   path, fences, Data window; add `scripts/make-data-window-image.py` to the storage `paths`),
   diagnostics (the new log lines). Run the `--base` record check first.
2. The push builds the kernel. Run the Data seed workflow (`size_mib=8192`), decrypt and unpack it
   into a directory of its own, **not** `~/Xnu4Sirius/seed/` (a seed there would be passed as a RAM
   seed by habit; the payload must not carry one now).
3. `python3 scripts/make-data-window-image.py --root <userdata-root.img> --container owner-data.cdr
   --output data-window.simg`, then from PowerShell `fastboot -S 512M flash userdata data-window.simg`.
   The FILL chunks write 8 GiB of zeros on the card, which takes minutes.
4. Build the payload without `--data-ramdisk`, flash `vendor`, capture. Expect `EMMC write window
   lba=35627008 …`, `NEMMC data window lba/blocks`, `NEMMC data volume (eMMC) /dev/…`, and no ENOSPC.

Not yet known: whether ABL's fastboot honours DONT_CARE without erasing `userdata` first. If the
next boot cannot mount the root, the root image was erased: flash `userdata-root.simg` again, then
the Data window image. The writes go to the phone's storage; keep both fences.

## After the Data volume

1. **Display**: iPadOS draws nothing. U-Boot leaves a 1080x2240 framebuffer at `0x9c000000`; the
   `GopFramebuffer` component (UEFI GOP to MobileFramebuffer) or NativeLegacyDisplay is the route
   the other ports took (asteroids, tb128fu).
2. **Touch**: Synaptics S3706; the OnePlus 6T (fajita) port has a driver to adapt.
3. **Storage speed**: the card supports DDR52 and HS200 (`EXT_CSD type 0x57`); DDR52 needs no tuning.
4. **Memory**: the user says this unit has 4 GB, while `board.toml` `dram_size` and the DTS memory
   node describe the 6 GB model. The 1 GiB window works (it starts at `0xa3…`); read the real map
   (DarwinBoot prints the free runs when a window does not fit) before asking for more.
5. **Upstream**: 57UU's `porting.toml` still says "PAN is not the cause" and its U-Boot pin breaks
   CI. A pull request with the PAN commits, the repin, the capture tooling and the eMMC work would
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

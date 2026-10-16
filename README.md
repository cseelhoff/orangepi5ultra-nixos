# orangepi5ultra-nixos

NixOS for the **Orange Pi 5 Ultra** (RK3588, aarch64). This flake provides:

| Output | What it is |
|---|---|
| `nixosModules.hardware` | Vendor 6.1 kernel (Armbian `rk-6.1-rkr5.1`) with **SDIO** Wi-Fi for the AP6611S, the Ultra device tree, Orange Pi firmware |
| `nixosModules.sd-card` | Partition layout and extlinux boot of the image (keep it imported after flashing) |
| `nixosModules.default` | Both of the above |
| `packages.aarch64-linux.sdImage` | A minimal, flashable `.img.zst` for microSD or NVMe |
| `packages.aarch64-linux.kernel` | The kernel alone (the slow part: ~70 min native on the board) |

The image is deliberately small: it boots, gets online, and lets you switch to
your real config. Personal settings, tools and desktops belong in that config
([cseelhoff/nixos-config](https://github.com/cseelhoff/nixos-config), host `opi5ultra`).

Board support comes from [gnull/nixos-rk3588](https://github.com/gnull/nixos-rk3588)
(its Orange Pi 5 Plus module), with the Ultra-specific overrides here.

## Prerequisite: U-Boot in SPI flash

The image contains **no bootloader**. The RK3588 boot ROM reads U-Boot from the
board's 16 MiB SPI NOR flash, which then loads `extlinux.conf` from the image's
`BOOT` partition. Check before flashing (read-only):

```sh
cat /proc/mtd                                   # expect: mtd0 ... "loader"
sudo strings -n 8 /dev/mtdblock0 | grep -m3 -E 'U-Boot [0-9]|SPL'
```

Known working: Orange Pi vendor `U-Boot SPL 2017.09-g5e34a2115` (Aug 2024).
If the SPI flash is empty, install a bootloader there first (Orange Pi's
`rkspi_loader.img`, mainline U-Boot or EDK2-RK3588). Removing the NixOS disk
always lets the board fall back to whatever else U-Boot can boot.

## Build

```sh
nix build .#sdImage            # -> result/sd-image/opi5ultra-nixos-*.img.zst
nix build .#kernel             # kernel only
```

Native on an aarch64 machine, or on x86_64 with
`boot.binfmt.emulatedSystems = [ "aarch64-linux" ];` (same store paths, slower).
With the Cachix cache configured, the kernel is downloaded instead of built.

### Why the kernel inputs are pinned by commit

`nixpkgs-kernel` and `nixos-rk3588` are pinned by revision **in `flake.nix`**,
not only in `flake.lock`. The kernel's store path is a hash of its whole build
recipe (compiler included), so any change to these two inputs means a full
rebuild and a cache miss. Pinning them in the URL means `nix flake update` only
moves the system packages (`nixpkgs`, nixos-26.05); kernel bumps are always a
deliberate edit:

1. Change the revisions in `flake.nix`, `nix flake lock`.
2. `nix build .#kernel` (and test on the board).
3. `cachix push cseelhoff-opi5ultra ./result`.

Consumers must **not** set `inputs.nixpkgs.follows` on this flake; the kernel
is always built from `nixpkgs-kernel`, whatever nixpkgs the host uses.

## Flash

Pick the target carefully; `dd` overwrites the whole device.

```sh
lsblk -o NAME,SIZE,MODEL,TRAN,MOUNTPOINTS
cat /sys/block/mmcblk0/device/type             # SD card reports "SD"
zstdcat result/sd-image/*.img.zst | sudo dd of=/dev/mmcblk0 bs=4M status=progress conv=fsync && sync
```

The same image works on an NVMe drive (`/dev/nvme0n1`) if your U-Boot boots NVMe.
**Never attach two disks flashed from this image at once:** every image has the
same root UUID (`14e19a7b-…`) and labels (`BOOT`, `NIXOS_SD`), so the kernel may
mount root from either one.

## First boot

- Serial console: `ttyS2` at **1500000** baud (also on HDMI `tty1`).
- Log in as `caleb` / `nixos` (console or SSH), then run `passwd` immediately.
  SSH also accepts the key in `image/configuration.nix`.
- Find it on the LAN as `opi5ultra.local` (Avahi), or `ip -br a` on the console.
- Root partition grows to fill the disk on first boot.
- Check Wi-Fi: `dmesg | grep -iE 'bcmdhd|dhd|wlan|firmware'`, `nmcli dev wifi list`.

Then switch to the full config:

```sh
git clone https://github.com/cseelhoff/nixos-config.git ~/nixos-config
cd ~/nixos-config && sudo nixos-rebuild switch --flake '.#opi5ultra'
```

## Binary cache (Cachix)

Public cache: `https://cseelhoff-opi5ultra.cachix.org`, key
`cseelhoff-opi5ultra.cachix.org-1:5YubECsqm2rE4P70yAwKxuCGAZ+EHyb9Ql227z4xlWE=`.

- **NixOS hosts** importing `nixosModules.hardware` (including the image) get it
  in `nix.settings` automatically.
- **Other machines** building this flake: `nix build` offers the flake's
  `nixConfig` (`--accept-flake-config`) if your user is in `trusted-users`;
  otherwise add the substituter and key to `/etc/nix/nix.conf`.
- **CI** pushes with the `CACHIX_AUTH_TOKEN` repository secret (a Write token
  from the cache's Settings → Auth Tokens).

Pushing by hand, after `cachix authtoken` (reads the token from stdin):

```sh
nix build .#kernel && cachix push cseelhoff-opi5ultra ./result
```

## Known issues and notes

- **Wi-Fi:** nixos-rk3588's kernel config builds `bcmdhd` for PCIe (5 Plus);
  `kernel.nix` switches it to SDIO for the Ultra's AP6611S.
- **Kernel command line** has both `root=UUID=…` and `root=fstab` (from
  upstream modules with the systemd initrd); both resolve to the same partition.
- The image's 32 MiB gap before `BOOT` leaves room to embed U-Boot later
  (sector 64), which would remove the SPI prerequisite.

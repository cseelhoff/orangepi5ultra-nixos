# arch-test: Orange Pi Arch baseline vs NixOS

Handoff for Claude Code running on **Orange Pi's Arch Linux (NVMe)** on the
Orange Pi 5 Ultra. Tell it: *"Read ~/arch-test/README.md and continue."*

## Context

Caleb is evaluating whether NixOS can replace this Arch install without
losing hardware capability (GPU, video capture, hardware codecs, NPU). NixOS
was built and tested from a microSD card. This directory holds the same
benchmark script and the NixOS results; the job here is to produce the
**Arch baseline** with the identical script and compare.

- Board repo (kernel, Wi-Fi fix, image): <https://github.com/cseelhoff/orangepi5ultra-nixos>
  (local clone: `~/orangepi5ultra-nixos`; its `bench/` folder mirrors this directory)
- Personal NixOS config, host `opi5ultra`: <https://github.com/cseelhoff/nixos-config> branch `opi5ultra`
- NixOS results: `results-nixos-*.txt` in this directory

## Safety rules (unchanged from the original session)

- This NVMe is the **running system**. Don't partition, format or `dd` anything.
- **Don't write to the SD card** (`mmcblk*`, labels `BOOT` / `NIXOS_SD`): it is the NixOS test install.
- **Never write to SPI flash** (`/dev/mtd*`).
- Ask before anything destructive; use sudo only where needed and say why.
- Prefer `nix shell nixpkgs#...` (Nix is installed here) over `pacman -S` for
  extra tools, to avoid partial-upgrade problems on Orange Pi's Arch repos.
  Don't use `pacman -Sy` without asking.

## ⚠️ Open hardware problem: NVMe dropout (read this first)

On 2026-10-16, while running NixOS from the SD card, the NVMe vanished from the
PCIe bus ~5 s after an all-core `stress-ng` load started:

```
nvme nvme0: controller is down; will reset: CSTS=0xffffffff, PCI_STATUS=0x10
nvme nvme0: Does your device have a faulty power saving mode enabled?
nvme nvme0: Removing after probe failure status: -19
```

**Suspected cause: electrical, not software.** The drive had been idle and
fine for an hour; it failed on a load step; the board has been **shorting
against its metal Orange Pi enclosure**, and the M.2 slot faces the base
plate; an HDMI cable had just been plugged in. The kernel's suggested
power-saving workaround (`nvme_core.default_ps_max_latency_us=0 pcie_aspm=off
pcie_port_pm=off`) was added to the NixOS board module **as an unconfirmed
precaution only**. It cannot fix a short or a weak power supply.

**Update 2026-10-16 21:06, after adding rubber stoppers under the case:**
same NixOS boot, still *without* the kernel workaround, 10 min of
`stress-ng --cpu 8` + `fio --readonly` random reads on the raw NVMe:
**no NVMe/PCIe errors**, drive present throughout, max 73 °C. Before the
stoppers it dropped 5 s into the same load. Strong evidence for the case
short; confirm with the write test below on Arch.

This must be settled before NixOS (or anything) relies on the NVMe as root.
Here, the NVMe is the root disk, so a dropout would crash Arch: **save work
and keep the stress test short at first.** Suggested test (ask Caleb first):

1. Note the power supply (should be a 5 V / 5 A USB-C supply) and whether the
   board is inside the metal case.
2. In one terminal: `sudo dmesg -w | grep -iE 'nvme|pcie|EXT4-fs'`
3. In another, I/O plus CPU load (fio writes a 1 GiB file in ~/arch-test/work):
   ```sh
   nix shell nixpkgs#fio nixpkgs#stress-ng -c sh -c '
     stress-ng --cpu 8 --cpu-method matrixprod -t 10m &
     fio --name=nvme --filename=$HOME/arch-test/work/fio-nvme.dat --size=1G \
         --rw=randrw --bs=64k --direct=1 --ioengine=libaio --iodepth=16 \
         --runtime=600 --time_based; wait'
   rm -f ~/arch-test/work/fio-nvme.dat
   ```
4. Repeat **outside the case** (or with the base plate insulated) and compare.
   A dropout inside but not outside the case confirms the short.

## Run the baseline benchmark

Same HDMI source connected to HDMI-in. The script is read-only outside this
directory and needs no root.

```sh
cd ~/arch-test
command -v glmark2 glmark2-es2 glmark2-es2-wayland glmark2-es2-drm ffmpeg v4l2-ctl fio stress-ng
# Use Arch's own glmark2 and ffmpeg (they carry the Rockchip GPU/MPP support);
# take only the generic tools from Nix. Do NOT add nixpkgs#ffmpeg, mesa-demos,
# glmark2 or vulkan-tools: those would use nixpkgs' Mesa and give NixOS numbers.
nix shell nixpkgs#fio nixpkgs#stress-ng nixpkgs#v4l-utils nixpkgs#iperf3 -c ./bench.sh
```

Optional: `IPERF_SERVER=<LAN host running iperf3 -s>` before `./bench.sh`.

Notes for interpreting:
- Arch's GPU stack is **mesa-panfork** on the vendor `mali` (kbase) driver, so
  glmark2 should produce a real score. The script tries `glmark2-es2-drm`
  first; with a desktop running, DRM is busy and it falls back to the
  Wayland/X11 binaries. Record which one ran.
- Arch's `ffmpeg` (7.0.2) is built with **rkmpp**, so the script also runs
  `h264_rkmpp` decode/encode. Watch that it really uses hardware (low CPU in
  `top`, `rtime` far below software).
- The NixOS run's single-core stress-ng hung on exit (timeout 124) around the
  moment the NVMe dropped; note whether that recurs here.

## Then

1. Write `comparison.md` here: a table of NixOS vs Arch per test, and for
   each gap, what NixOS would need. Known NixOS gaps:
   - **GPU:** no acceleration (nixpkgs Mesa can't drive kbase). Fix routes:
     bind the open `panthor` driver (compiled into the kernel; needs DT +
     firmware) or package a kbase userspace.
   - **Codecs:** no Rockchip MPP / RGA / ffmpeg-rkmpp in nixpkgs.
   - **NPU:** RKNPU driver loads; no RKNN userspace in nixpkgs (Arch has none
     installed either).
   - HDMI capture: **parity expected** (NixOS: 4K60 NV12 at 60.00 fps).
2. Report the NVMe stability test result.
3. Don't commit or push anything without asking Caleb.

## Files

| File | What |
|---|---|
| `bench.sh` | The benchmark (same script on both OSes) |
| `results-nixos-*.txt` | NixOS results (SD card, kernel 6.1.115 vendor) |
| `results-arch-*.txt` | Written by `bench.sh` here |
| `work/` | Scratch: test clips, HDMI frame, fio files. Safe to delete |

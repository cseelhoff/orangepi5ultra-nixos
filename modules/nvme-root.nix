# Root filesystem and boot for an install on the NVMe (instead of sd-card.nix).
#
# Layout (same as Orange Pi's own NVMe images, which the SPI U-Boot boots):
# MBR, one Linux partition from 32 MiB to the end, ext4, label NIXOS_NVME,
# with the fixed UUID below. The bootloader config lives inside it at
# /boot/extlinux/extlinux.conf (generic-extlinux), like on the SD card.
#
# Prepare and install (from another system on this board, e.g. the SD card):
#   sfdisk /dev/nvme0n1 <<< 'label: dos
#   start=32MiB, type=83'
#   mkfs.ext4 -L NIXOS_NVME -U 8b91ff92-e62b-4947-9b2b-51f107172fb4 /dev/nvme0n1p1
#   mount /dev/nvme0n1p1 /mnt
#   nixos-install --root /mnt --system $(nix build --print-out-paths <flake>#nixosConfigurations.<host>.config.system.build.toplevel)
#
# Boot order: the SPI U-Boot tries the SD card before the NVMe, so remove the
# SD card (or keep it as a fallback that wins when inserted).
{ lib, ... }:
let
  rootUUID = "8b91ff92-e62b-4947-9b2b-51f107172fb4";
in
{
  fileSystems."/" = {
    device = "/dev/disk/by-uuid/${rootUUID}";
    fsType = "ext4";
  };

  boot = {
    kernelParams = [
      "root=UUID=${rootUUID}"
      "rootfstype=ext4"
    ];
    loader = {
      grub.enable = lib.mkForce false;
      generic-extlinux-compatible.enable = lib.mkForce true;
    };
  };
}

# Disk layout and boot for the flashed image, following nixos-rk3588's 5 Plus
# sd-image module. Imports the sd-image module from the *host's* nixpkgs
# (modulesPath), so it works with whatever release the importing config uses.
#
# Layout: 32 MiB gap (room for a bootloader) | BOOT vfat 200 MiB (extlinux,
# kernel, dtbs) | NIXOS_SD ext4 root, expanded to fill the disk on first boot.
# U-Boot itself is NOT in the image: the board's SPI flash must provide it.
{ config, lib, modulesPath, ... }:
let
  # Fixed, so every flashed disk has the same root UUID. Never have two disks
  # flashed from this image attached to one board at the same time.
  rootPartitionUUID = "14e19a7b-0ae0-484d-9d54-43bd6fdc20c7";
in
{
  imports = [ "${modulesPath}/installer/sd-card/sd-image-aarch64.nix" ];

  boot = {
    kernelParams = [
      "root=UUID=${rootPartitionUUID}"
      "rootfstype=ext4"
    ];
    loader = {
      grub.enable = lib.mkForce false;
      generic-extlinux-compatible.enable = lib.mkForce true;
    };
  };

  image.baseName = "opi5ultra-nixos";

  sdImage = {
    inherit rootPartitionUUID;
    compressImage = true;
    populateFirmwareCommands = ''
      ${config.boot.loader.generic-extlinux-compatible.populateCmd} -c ${config.system.build.toplevel} -d ./firmware
    '';
    firmwarePartitionOffset = 32;
    firmwarePartitionName = "BOOT";
    firmwareSize = 200;
    populateRootCommands = ''
      mkdir -p ./files/boot
      mkdir -p ./files/boot/firmware
    '';
  };
}

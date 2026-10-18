# The Orange Pi 5 Ultra's AP6611S Wi-Fi sits on SDIO, but nixos-rk3588's vendor
# kernel config (written for the 5 Plus's AP6275P) builds bcmdhd for PCIe only,
# so the module never binds to the chip. Rebuild with the SDIO variant, matching
# Orange Pi's own 5.10 kernel. Firmware (fw_syn43711a0_sdio.bin, nvram_ap6611s.txt,
# clm_syn43711a0.blob) already comes from the board's orangepi-firmware package.
#
# Everything here is built from pkgsKernel (the pinned nixpkgs-kernel input), never
# from the host's pkgs, so the kernel's store path is the same in the image, in
# any host config that imports this flake, and in the binary cache.
{ pkgsKernel, nixos-rk3588 }:
let
  sdioManualConfig = args: pkgsKernel.linuxManualConfig (args // {
    configfile = pkgsKernel.runCommand "rk35xx_vendor_config-sdio" { } ''
      sed -e 's|^# CONFIG_BCMDHD_SDIO is not set$|CONFIG_BCMDHD_SDIO=y|' \
          -e 's|^CONFIG_BCMDHD_PCIE=y$|# CONFIG_BCMDHD_PCIE is not set|' \
          -e 's|^CONFIG_BCMDHD_FW_PATH=.*|CONFIG_BCMDHD_FW_PATH="/lib/firmware/fw_bcmdhd.bin"|' \
          -e 's|^CONFIG_BCMDHD_NVRAM_PATH=.*|CONFIG_BCMDHD_NVRAM_PATH="/lib/firmware/nvram.txt"|' \
          ${args.configfile} > $out
      grep -qx 'CONFIG_BCMDHD_SDIO=y' $out
    '';
    # Armbian's 78c67d98f221 drops capture from every hdmi-codec user to fix
    # HDMI-out mono audio; that also removed the HDMI receiver's only stream.
    kernelPatches = (args.kernelPatches or [ ]) ++ [{
      name = "hdmi-codec-keep-capture-for-hdmirx";
      patch = ./patches/hdmi-codec-keep-capture-for-hdmirx.patch;
    }];
    config = (removeAttrs args.config [ "CONFIG_BCMDHD_PCIE" ]) // {
      CONFIG_BCMDHD_SDIO = "y";
    };
  });
in
pkgsKernel.callPackage "${nixos-rk3588}/pkgs/kernel/vendor.nix" {
  linuxManualConfig = sdioManualConfig;
}

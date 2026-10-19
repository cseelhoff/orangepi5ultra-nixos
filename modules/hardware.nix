# Orange Pi 5 Ultra hardware support: vendor 6.1 kernel with SDIO Wi-Fi,
# the Ultra device tree, Orange Pi firmware and RK3588 boot settings.
# Takes its kernel inputs from the board flake, so importing hosts need no
# specialArgs and their nixpkgs version does not change the kernel.
{ nixos-rk3588, nixpkgs-kernel, pkgsKernel, kernel }:
{ lib, pkgs, ... }:
{
  # Board module from nixos-rk3588, written for the 5 Plus (same RK3588 SoC and
  # vendor kernel); the Ultra-specific bits are overridden below.
  imports = [ nixos-rk3588.nixosModules.boards.orangepi5plus.core ];

  _module.args.rk3588 = {
    nixpkgs = nixpkgs-kernel;
    inherit pkgsKernel;
  };

  nixpkgs.hostPlatform = lib.mkDefault "aarch64-linux";

  # The kernel is not on cache.nixos.org; fetch it from this flake's Cachix.
  nix.settings = {
    substituters = [ "https://cseelhoff-opi5ultra.cachix.org" ];
    trusted-public-keys = [ "cseelhoff-opi5ultra.cachix.org-1:5YubECsqm2rE4P70yAwKxuCGAZ+EHyb9Ql227z4xlWE=" ];
  };

  boot.kernelPackages = lib.mkForce (pkgsKernel.linuxPackagesFor kernel);
  hardware.deviceTree.name = lib.mkForce "rockchip/rk3588-orangepi-5-ultra.dtb";

  # The vendor DT gives HDMI-RX's "hdmi" interrupt as SPI 436, a line only
  # Rockchip's own BL31 raises (it takes the real SPI 178 as FIQ and forwards
  # it). The SPI flash now runs upstream TF-A, so use the real line, as
  # mainline does. Without it the receiver never saw source mode changes or
  # audio except through the polling fallback in
  # patches/hdmirx-without-vendor-tfa.patch.
  hardware.deviceTree.overlays = [{
    name = "hdmirx-irq-spi178";
    dtsText = ''
      /dts-v1/;
      /plugin/;
      / {
        /* only this board: other RK3588 DTBs in the kernel lack the node path */
        compatible = "rockchip,rk3588-orangepi-5-ultra";
        fragment@0 {
          target-path = "/hdmirx-controller@fdee0000";
          __overlay__ {
            /* GIC_SPI 177/178/179, IRQ_TYPE_LEVEL_HIGH: cec, hdmi, dma */
            interrupts = <0 177 4>, <0 178 4>, <0 179 4>;
          };
        };
      };
    '';
  }];

  # ======================================================================
  # !!! UNCONFIRMED WORKAROUND. THE REAL CAUSE IS PROBABLY ELECTRICAL. !!!
  # ======================================================================
  # 2026-10-16: the NVMe (DRAM-less NE-256, host memory buffer) dropped off
  # the PCIe bus: "controller is down; will reset: CSTS=0xffffffff".
  # The kernel's message blames NVMe power saving, and these parameters are
  # its suggested workaround. BUT the evidence points elsewhere:
  #   - the drive sat idle for ~1 h without problems (APST bugs hit at idle);
  #   - it dropped ~5 s after an all-core stress-ng load started (power step);
  #   - the board had been shorting against its metal Orange Pi enclosure,
  #     and the M.2 slot sits on the underside against the base plate;
  #   - an HDMI cable had just been plugged in (board handled in the case).
  # Treat this as a power/short problem until proven otherwise: test outside
  # the enclosure and with a known-good 5 V / 5 A USB-C supply. These params
  # only stop link power saving; they CANNOT fix a short or a sagging rail.
  # Remove them once the electrical cause is found and fixed, then re-test.
  # ======================================================================
  boot.kernelParams = [
    "nvme_core.default_ps_max_latency_us=0"
    "pcie_aspm=off"
    "pcie_port_pm=off"
  ];

  # The AP6611S's bcmdhd driver scans unreliably with MAC address
  # randomization and power saving on: `nmcli device wifi list` can come back
  # empty even with APs in range and connected to from another OS. Disabling
  # both (no effect unless networking.networkmanager.enable = true) fixed it.
  networking.networkmanager.wifi = {
    scanRandMacAddress = false;
    macAddress = "permanent";
    powersave = false;
  };

  # Bluetooth: the AP6611S's BT side sits on UART7 (/dev/ttyS7). The vendor DT
  # only powers it (rfkill "bt_default"); nothing attaches the HCI, so start
  # btattach at boot. The 6.1 kernel's btbcm does not know the SYN43711 name
  # and asks for brcm/BCM.hcd, so provide the Synaptics patch under that name;
  # without it the chip runs its ROM firmware and finds no devices.
  # Tested 2026-10-18: firmware "SYN43711 UART 37.4MHz AMPAK AP6611", LE scan
  # finds nearby devices.
  hardware.bluetooth.enable = lib.mkDefault true;
  hardware.firmware = [
    (pkgs.runCommand "ap6611s-bt-firmware" { } ''
      mkdir -p $out/lib/firmware/brcm
      cp ${pkgs.callPackage "${nixos-rk3588}/pkgs/orangepi-firmware" { }}/lib/firmware/SYN43711A0.hcd \
        $out/lib/firmware/brcm/BCM.hcd
    '')
  ];

  # The vendor bcmdhd driver opens its firmware by absolute path
  # (/lib/firmware/fw_syn43711a0_sdio.bin, config_*.txt), not through the
  # kernel firmware loader, so NixOS's /run/current-system/firmware is not
  # searched; without this wlan0 fails with "fwpkg_init failed" when brought up.
  systemd.tmpfiles.rules = [
    "L+ /lib/firmware - - - - /run/current-system/firmware"
  ];

  systemd.services.ap6611s-bluetooth = {
    description = "Attach the AP6611S Bluetooth controller on UART7";
    wantedBy = [ "bluetooth.target" "multi-user.target" ];
    before = [ "bluetooth.service" ];
    after = [ "dev-ttyS7.device" ];
    serviceConfig = {
      ExecStartPre = "${pkgs.util-linux}/bin/rfkill unblock bluetooth";
      ExecStart = "${pkgs.bluez}/bin/btattach -B /dev/ttyS7 -P bcm -S 1500000";
      Restart = "on-failure";
      RestartSec = 5;
    };
  };
}

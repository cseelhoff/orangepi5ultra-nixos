# Orange Pi 5 Ultra hardware support: vendor 6.1 kernel with SDIO Wi-Fi,
# the Ultra device tree, Orange Pi firmware and RK3588 boot settings.
# Takes its kernel inputs from the board flake, so importing hosts need no
# specialArgs and their nixpkgs version does not change the kernel.
{ nixos-rk3588, nixpkgs-kernel, pkgsKernel, kernel }:
{ lib, ... }:
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
}

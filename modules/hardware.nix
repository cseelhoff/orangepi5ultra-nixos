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
}

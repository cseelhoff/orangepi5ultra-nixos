{
  description = "NixOS for the Orange Pi 5 Ultra (RK3588): hardware module + flashable SD/NVMe image";

  inputs = {
    # System packages for the image. Matches cseelhoff/nixos-config so the
    # first `nixos-rebuild` after flashing is not a release change.
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";

    # Kernel toolchain. Pinned BY COMMIT so `nix flake update` never changes the
    # kernel's store path (which would mean a ~70 min rebuild and a cache miss).
    # Bump these two deliberately, rebuild the kernel, and push it to Cachix.
    nixpkgs-kernel.url = "github:NixOS/nixpkgs/55bfb4d7437bd4e09ab7cba58a43bd68eb310044";
    nixos-rk3588 = {
      url = "github:gnull/nixos-rk3588/2a1add82960dda2e0d203051dcf1ae4c1bc8452c";
      inputs.nixpkgs.follows = "nixpkgs-kernel";
    };
  };

  # Prebuilt kernel for `nix build` of this flake on any machine. (Hosts that
  # import nixosModules.hardware get the same cache via nix.settings.)
  nixConfig = {
    extra-substituters = [ "https://cseelhoff-opi5ultra.cachix.org" ];
    extra-trusted-public-keys = [ "cseelhoff-opi5ultra.cachix.org-1:5YubECsqm2rE4P70yAwKxuCGAZ+EHyb9Ql227z4xlWE=" ];
  };

  outputs = { self, nixpkgs, nixpkgs-kernel, nixos-rk3588, ... }:
    let
      system = "aarch64-linux";
      pkgsKernel = import nixpkgs-kernel { inherit system; };
      kernel = import ./kernel.nix { inherit pkgsKernel nixos-rk3588; };
    in
    {
      nixosModules = {
        # Kernel, device tree, firmware, Wi-Fi. Import this from any host config.
        hardware = import ./modules/hardware.nix { inherit nixos-rk3588 nixpkgs-kernel pkgsKernel kernel; };
        # Partition layout + extlinux boot of the image. Keep it imported after
        # flashing: it defines fileSystems and the bootloader.
        sd-card = ./modules/sd-card.nix;
        default = {
          imports = [ self.nixosModules.hardware self.nixosModules.sd-card ];
        };
      };

      # Minimal bootable system: enough to log in, get online and run
      # `nixos-rebuild` against your real config.
      nixosConfigurations.opi5ultra = nixpkgs.lib.nixosSystem {
        modules = [
          self.nixosModules.default
          ./image/configuration.nix
        ];
      };

      packages.${system} = {
        inherit kernel;
        sdImage = self.nixosConfigurations.opi5ultra.config.system.build.sdImage;
        default = self.packages.${system}.sdImage;
      };
    };
}

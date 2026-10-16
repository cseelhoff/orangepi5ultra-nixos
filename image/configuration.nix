# The flashed base system: just enough to log in, get online and switch to a
# real config (`nixos-rebuild switch --flake github:cseelhoff/nixos-config#opi5ultra`).
# Test tools, desktop, etc. belong in that config, not here.
{ pkgs, ... }:
{
  networking.hostName = "opi5ultra";
  networking.networkmanager.enable = true;
  time.timeZone = "America/Chicago";

  # Reachable as opi5ultra.local without knowing its IP.
  services.avahi = {
    enable = true;
    nssmdns4 = true;
    publish = {
      enable = true;
      addresses = true;
    };
  };

  nix.settings.experimental-features = [ "nix-command" "flakes" ];
  # Prebuilt kernel for the first nixos-rebuild. Uncomment with flake.nix's nixConfig.
  # nix.settings.extra-substituters = [ "https://cseelhoff-opi5ultra.cachix.org" ];
  # nix.settings.extra-trusted-public-keys = [ "cseelhoff-opi5ultra.cachix.org-1:REPLACE_ME" ];

  services.openssh = {
    enable = true;
    settings = {
      PasswordAuthentication = true;
      KbdInteractiveAuthentication = true;
      PermitRootLogin = "no";
    };
  };

  users.users.caleb = {
    isNormalUser = true;
    extraGroups = [ "wheel" "networkmanager" "video" "audio" "render" ];
    # Publicly known default: change it with `passwd` on first login.
    initialPassword = "nixos";
    openssh.authorizedKeys.keys = [
      "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIDexbF/Hyk975Dn0M5Y9455dXEMzkEtp3ksXVeGAhEbF"
    ];
  };

  environment.systemPackages = with pkgs; [
    git
    htop
  ];

  system.stateVersion = "26.05";
}

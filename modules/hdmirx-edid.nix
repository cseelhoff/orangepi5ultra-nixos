# Keep the HDMI-in EDID in step with the HDMI-out monitor: at boot and on every
# HDMI-out hotplug, the receiver advertises the monitor's manufacturer, model,
# serial and name (generic "LNX Generic 4K60" when nothing is connected).
{ pkgs, ... }:
let
  hdmirx-edid-sync = pkgs.callPackage ../pkgs/hdmirx-edid-sync.nix { };
in
{
  environment.systemPackages = [ hdmirx-edid-sync ];

  systemd.services.hdmirx-edid-sync = {
    description = "Mirror the HDMI-out monitor identity into the HDMI-in EDID";
    wantedBy = [ "multi-user.target" ];
    after = [ "systemd-udev-settle.service" ];
    serviceConfig = {
      Type = "oneshot";
      ExecStart = "${hdmirx-edid-sync}/bin/hdmirx-edid-sync";
    };
  };

  # HDMI-out hotplug arrives as a "change" event on the DRM card.
  services.udev.extraRules = ''
    ACTION=="change", SUBSYSTEM=="drm", KERNEL=="card[0-9]*", ENV{HOTPLUG}=="1", RUN+="${pkgs.systemd}/bin/systemctl --no-block restart hdmirx-edid-sync.service"
  '';
}

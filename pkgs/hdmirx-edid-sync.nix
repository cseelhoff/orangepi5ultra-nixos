# hdmirx-edid-sync: HDMI-in EDID = base capabilities + identity of the HDMI-out monitor.
# The base is the receiver's built-in default (RK-UHD, 4K60 + audio), captured 2026-10-18.
{ lib, writers, v4l-utils }:

writers.writePython3Bin "hdmirx-edid-sync"
  {
    flakeIgnore = [ "E501" ];
    makeWrapperArgs = [
      "--prefix" "PATH" ":" (lib.makeBinPath [ v4l-utils ])
      "--set-default" "HDMIRX_EDID_BASE" "${./hdmirx-edid-base.bin}"
    ];
  }
  (builtins.readFile ./hdmirx-edid-sync.py)

# Hardware video encode/decode with the vendor kernel's MPP driver: Rockchip
# MPP userspace, the GStreamer mpp* elements and an HDMI-in recorder.
# Tested 2026-10-18: live 4K60 HDMI-in -> H.264/H.265 at 60 fps, ~0.2 CPU core.
{ pkgs, ... }:
let
  rockchip-mpp = pkgs.callPackage ../pkgs/rockchip-mpp.nix { };
  librga = pkgs.callPackage ../pkgs/librga.nix { };
  rga-bench = pkgs.callPackage ../pkgs/rga-bench.nix { inherit librga; };
  gst-rockchip = pkgs.callPackage ../pkgs/gst-rockchip.nix { inherit rockchip-mpp librga; };
  hdmirx-record = pkgs.callPackage ../pkgs/hdmirx-record.nix { inherit gst-rockchip; };
  hdmi-passthru = pkgs.callPackage ../pkgs/hdmi-passthru.nix { inherit rockchip-mpp librga; };
  hdmirx-audio-stream = pkgs.callPackage ../pkgs/hdmirx-audio-stream.nix { };
in
{
  # The encoder, RGA and DMA heaps are root-only (0600) by default.
  services.udev.extraRules = ''
    KERNEL=="mpp_service", MODE="0660", GROUP="video"
    KERNEL=="rga", MODE="0660", GROUP="video"
    SUBSYSTEM=="dma_heap", MODE="0660", GROUP="video"
    # HDMI-in audio: the receiver starts its I2S output ~50 ms after capture
    # starts, exactly the I2S read timeout in Rockchip's DT, so every capture
    # failed with EIO. Give it 500 ms.
    SUBSYSTEM=="sound", KERNEL=="controlC*", ATTRS{id}=="rockchiphdmiin", RUN+="${pkgs.alsa-utils}/bin/amixer -q -c %n cset iface=PCM,name='PCM Read Wait Time MS' 500"
  '';

  # hdmi-passthru switches HDMI-in to low-latency hand-out (each frame given out
  # at input line 10) while it runs and restores it on exit. It stays off
  # otherwise: an encoder reading a frame that is still arriving would record tears.
  systemd.tmpfiles.rules = [
    "z /sys/module/rockchip_hdmirx/parameters/low_latency 0664 root video -"
  ];

  environment.systemPackages = [
    rockchip-mpp # mpi_enc_test, mpi_dec_test, ...
    gst-rockchip
    hdmirx-record
    hdmi-passthru # HDMI-in -> HDMI-out with a text overlay, ~1 ms
    hdmirx-audio-stream # HDMI-in audio -> Opus over RTP/SRT
    librga
    rga-bench # RGA scale/convert throughput
    pkgs.gst_all_1.gstreamer
    pkgs.v4l-utils
  ];

  # Lets plain gst-launch-1.0 / gst-inspect-1.0 find the mpp* elements.
  environment.sessionVariables.GST_PLUGIN_SYSTEM_PATH_1_0 = map (p: "${p}/lib/gstreamer-1.0") [
    pkgs.gst_all_1.gstreamer.out
    pkgs.gst_all_1.gst-plugins-base
    pkgs.gst_all_1.gst-plugins-good
    pkgs.gst_all_1.gst-plugins-bad
    gst-rockchip
  ];
}

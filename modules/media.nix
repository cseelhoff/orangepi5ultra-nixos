# Hardware video encode/decode with the vendor kernel's MPP driver: Rockchip
# MPP userspace, the GStreamer mpp* elements and an HDMI-in recorder.
# Tested 2026-10-18: live 4K60 HDMI-in -> H.264/H.265 at 60 fps, ~0.2 CPU core.
{ pkgs, ... }:
let
  rockchip-mpp = pkgs.callPackage ../pkgs/rockchip-mpp.nix { };
  gst-rockchip = pkgs.callPackage ../pkgs/gst-rockchip.nix { inherit rockchip-mpp; };
  hdmirx-record = pkgs.callPackage ../pkgs/hdmirx-record.nix { inherit gst-rockchip; };
in
{
  # The encoder, RGA and DMA heaps are root-only (0600) by default.
  services.udev.extraRules = ''
    KERNEL=="mpp_service", MODE="0660", GROUP="video"
    KERNEL=="rga", MODE="0660", GROUP="video"
    SUBSYSTEM=="dma_heap", MODE="0660", GROUP="video"
  '';

  environment.systemPackages = [
    rockchip-mpp # mpi_enc_test, mpi_dec_test, ...
    gst-rockchip
    hdmirx-record
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

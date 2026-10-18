# GStreamer elements for Rockchip MPP: mpph264enc, mpph265enc, mppjpegenc,
# mppvp8enc, mppvideodec. RGA, GPU conversion, rkximage and kmssrc are off
# (not needed for v4l2src -> encoder with NV12 input).
{ lib, stdenv, fetchFromGitHub, meson, ninja, pkg-config, gst_all_1, rockchip-mpp }:

stdenv.mkDerivation {
  pname = "gst-rockchip";
  version = "1.14.4-unstable-2026-10-16";

  src = fetchFromGitHub {
    owner = "JeffyCN";
    repo = "mirrors";
    rev = "890834cccdda8dea7baa417939840c08ed6826d1"; # branch gstreamer-rockchip
    hash = "sha256-xhJVSjOhxatg9QawPqwxj8cOT2Q3PpNN8AoWzwkW4W0=";
  };

  # Without this, the encoder asks v4l2src for no spare buffers and live 4K60
  # capture drops to 30 fps (each frame waits for the previous encode).
  patches = [ ./gst-rockchip-min-buffers.patch ];

  nativeBuildInputs = [ meson ninja pkg-config ];
  buildInputs = [ gst_all_1.gstreamer gst_all_1.gst-plugins-base rockchip-mpp ];

  mesonFlags = [
    "-Drockchipmpp=enabled"
    "-Drga=disabled"
    "-Dgpu=disabled"
    "-Drkximage=disabled"
    "-Dkmssrc=disabled"
    "-Dvpxalphadec=disabled"
  ];

  meta = {
    description = "GStreamer plugin for Rockchip MPP hardware video encode/decode";
    homepage = "https://github.com/JeffyCN/mirrors/tree/gstreamer-rockchip";
    license = lib.licenses.lgpl21Plus;
    platforms = [ "aarch64-linux" ];
  };
}

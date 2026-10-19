# hdmi-passthru: HDMI-in -> HDMI-out with a translucent text overlay, zero copy,
# phase-locked to the input (~1 ms per line with low-latency hand-out), plus
# optional hardware JPEG snapshots (RGA scale + MPP JPEG on a worker thread).
{ stdenv, pkg-config, libdrm, cairo, rockchip-mpp, librga }:

stdenv.mkDerivation {
  pname = "hdmi-passthru";
  version = "0.2";
  src = ../tools/hdmi-passthru;
  nativeBuildInputs = [ pkg-config ];
  buildInputs = [ libdrm cairo rockchip-mpp librga ];
  buildPhase = ''
    $CXX -O2 -Wall -c snapshot.cpp -o snapshot.o
    $CC -O2 -Wall -pthread -c hdmi-passthru.c -o hdmi-passthru.o $(pkg-config --cflags libdrm cairo)
    $CC -O2 -Wall -pthread -c novl.c -o novl.o
    $CXX -o hdmi-passthru hdmi-passthru.o novl.o snapshot.o \
      $(pkg-config --libs libdrm cairo rockchip_mpp librga) -lpthread
  '';
  installPhase = ''
    install -Dm755 hdmi-passthru $out/bin/hdmi-passthru
  '';
  meta.platforms = [ "aarch64-linux" ];
}

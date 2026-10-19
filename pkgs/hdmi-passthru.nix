# hdmi-passthru: HDMI-in -> HDMI-out with a translucent text overlay, zero copy,
# phase-locked to the input. ~1 ms per line with low-latency hand-out.
{ stdenv, pkg-config, libdrm, cairo }:

stdenv.mkDerivation {
  pname = "hdmi-passthru";
  version = "0.1";
  src = ../tools/hdmi-passthru;
  nativeBuildInputs = [ pkg-config ];
  buildInputs = [ libdrm cairo ];
  buildPhase = ''
    $CC -O2 -Wall -pthread -o hdmi-passthru hdmi-passthru.c novl.c $(pkg-config --cflags --libs libdrm cairo)
  '';
  installPhase = ''
    install -Dm755 hdmi-passthru $out/bin/hdmi-passthru
  '';
  meta.platforms = [ "aarch64-linux" ];
}

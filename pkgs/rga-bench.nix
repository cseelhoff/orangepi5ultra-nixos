# rga-bench: RGA 2D engine throughput (scale / colour convert on dma-bufs).
{ stdenv, librga }:

stdenv.mkDerivation {
  pname = "rga-bench";
  version = "0.1";
  src = ../tools/rga-bench;
  buildInputs = [ librga ];
  buildPhase = ''
    $CXX -O2 -o rga-bench rga-bench.cpp -I${librga}/include -lrga
  '';
  installPhase = ''
    install -Dm755 rga-bench $out/bin/rga-bench
  '';
  meta.platforms = [ "aarch64-linux" ];
}

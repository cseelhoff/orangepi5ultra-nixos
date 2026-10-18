# Rockchip RGA 2D engine userspace (librga / im2d) for the vendor kernel's
# multi-RGA driver (/dev/rga: 2x RGA3 + RGA2 on RK3588). Built from source.
{ lib, stdenv, fetchFromGitHub, meson, ninja, pkg-config, libdrm }:

stdenv.mkDerivation {
  pname = "librga";
  version = "1.10.5-unstable-2026-10-18";

  src = fetchFromGitHub {
    owner = "JeffyCN";
    repo = "mirrors";
    rev = "57a1067a246c71fa6c9a355d1668884fda155dd5"; # branch linux-rga-multi
    hash = "sha256-MXjKSOyu5St/SRVSoGqatxhGKsE3khiuLyeuTRoeAJU=";
  };

  nativeBuildInputs = [ meson ninja pkg-config ];
  buildInputs = [ libdrm ];
  mesonFlags = [ "-Dlibdrm=true" "-Dlibrga_demo=false" ];

  meta = {
    description = "Rockchip RGA 2D acceleration library";
    homepage = "https://github.com/airockchip/librga";
    license = lib.licenses.asl20;
    platforms = [ "aarch64-linux" ];
  };
}

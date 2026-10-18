# Rockchip Media Process Platform: userspace for the vendor kernel's mpp_service
# (rkvenc2 H.264/H.265 encode, rkvdec2 decode). Includes the mpi_*_test tools.
{ lib, stdenv, fetchFromGitHub, cmake }:

stdenv.mkDerivation {
  pname = "rockchip-mpp";
  version = "1.3.10-unstable-2026-10-16";

  src = fetchFromGitHub {
    owner = "rockchip-linux";
    repo = "mpp";
    rev = "20959ec3dabb313b8c33ca0b7b09c06ffd2a9caa";
    hash = "sha256-G9O6KGVuUCvX8w8q6Lc7ghhduUAqNJxjQEpNXe9kZfA=";
  };

  # The .pc templates prepend ''${prefix}/ to CMAKE_INSTALL_*DIR, which Nix sets absolute.
  postPatch = ''
    sed -i -e 's|''${prefix}/@CMAKE_INSTALL_LIBDIR@|@CMAKE_INSTALL_FULL_LIBDIR@|' \
           -e 's|''${prefix}/@CMAKE_INSTALL_INCLUDEDIR@|@CMAKE_INSTALL_FULL_INCLUDEDIR@|' \
           pkgconfig/*.pc.cmake
  '';

  nativeBuildInputs = [ cmake ];
  cmakeFlags = [ "-DBUILD_TEST=ON" ];

  meta = {
    description = "Rockchip Media Process Platform (hardware video encode/decode)";
    homepage = "https://github.com/rockchip-linux/mpp";
    license = with lib.licenses; [ asl20 mit ];
    platforms = [ "aarch64-linux" ];
  };
}

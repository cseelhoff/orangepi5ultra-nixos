# Arm's closed Mali-G610 userspace driver (Rockchip build, g24p0) for the vendor
# kernel's kbase driver (DDK g25p0). One library provides EGL, GLES 1/2/3, GBM,
# wayland-egl, OpenCL (ICD) and Vulkan (ICD).
#
# Built with the upstream meson script as a "vendor package": the GL/GBM wrapper
# libraries go to $out/lib/mali so they never shadow Mesa; use them with
# LD_LIBRARY_PATH=$out/lib/mali (see the libmali-run wrapper in modules/gpu.nix).
# OpenCL and Vulkan are found through their ICD files.
{ lib, stdenv, fetchFromGitHub, meson, ninja, pkg-config, autoPatchelfHook
, libdrm, wayland }:

let
  gpu = "valhall-g610";
  version = "g24p0";
  platform = "wayland-gbm";
in
stdenv.mkDerivation {
  pname = "libmali-${gpu}-${platform}";
  inherit version;

  # The branch carries every GPU and platform (several GB); fetch only ours.
  src = fetchFromGitHub {
    owner = "JeffyCN";
    repo = "mirrors";
    rev = "34f626298b0e8f884545365b66bb47678266e8d7"; # branch libmali
    nonConeMode = true;
    sparseCheckout = [
      "/meson.build"
      "/meson_options.txt"
      "/END_USER_LICENCE_AGREEMENT.txt"
      "/scripts/"
      "/hook/"
      "/include/"
      "/data/"
      "/firmware/g610/"
      "/optimize_3"
      "/lib/aarch64-linux-gnu/libmali-${gpu}-${version}-${platform}.so"
    ];
    hash = "sha256-mu6BUaQp0n67aw8qG3ZbX6biXqywJLHUmY7KmX5ofDo=";
  };

  postPatch = ''
    patchShebangs scripts
    substituteInPlace meson.build \
      --replace-fail "'/etc/ld.so.conf.d'" "get_option('sysconfdir') / 'ld.so.conf.d'"
  '';

  nativeBuildInputs = [ meson ninja pkg-config autoPatchelfHook ];
  buildInputs = [ libdrm wayland stdenv.cc.cc.lib ];
  # meson links test programs against the prebuilt library; let ld find its
  # own dependencies (libdrm, wayland) when doing so.
  env.NIX_LDFLAGS = "-rpath-link ${lib.getLib libdrm}/lib -rpath-link ${lib.getLib wayland}/lib";

  # The ICD files name the libraries without a path.
  postInstall = ''
    echo $out/lib/mali/libMaliOpenCL.so.1 > $out/etc/OpenCL/vendors/mali.icd
    substituteInPlace $out/share/vulkan/icd.d/mali.json \
      --replace-fail '"libMaliVulkan.so.1"' "\"$out/lib/mali/libMaliVulkan.so.1\""
  '';

  mesonFlags = [
    "-Dgpu=${gpu}"
    "-Dversion=${version}"
    "-Dplatform=${platform}"
    "-Dvendor-package=true"
    "-Dwrappers=enabled"
    "-Dopencl-icd=true"
    "-Dkhr-header=true"
    "-Dfirmware-dir=${placeholder "out"}/lib/firmware"
  ];

  meta = {
    description = "Mali-G610 closed userspace driver (EGL, GLES, OpenCL, Vulkan) for the Rockchip vendor kernel";
    homepage = "https://github.com/JeffyCN/mirrors/tree/libmali";
    license = lib.licenses.unfreeRedistributable;
    platforms = [ "aarch64-linux" ];
  };
}

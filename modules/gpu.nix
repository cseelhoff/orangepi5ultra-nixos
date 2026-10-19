# Mali-G610 acceleration on the vendor kernel through Arm's closed libmali
# (Mesa's panfrost/panthor do not talk to the vendor kbase driver).
#   OpenCL 3.0 and Vulkan 1.3: system-wide through their ICD files.
#   EGL / GLES 3.2: system-wide through libglvnd (libEGL_mali adapter), so
#   unmodified programs use the GPU. glvnd dispatch costs ~0.03 us per GL call,
#   the same path every Mesa program takes.
#   GBM: a Mesa GBM backend (rockchip_gbm.so) hands buffer allocation to
#   libmali, so compositors and other direct-to-display programs using the
#   system libgbm work unmodified (kmscube 60 fps on HDMI-out).
#   `libmali-run <program>` remains as a fallback that swaps in libmali's libgbm.
# Tested 2026-10-18: clpeak fp32 492 GFLOPS, fp16 964, int8 dot 1934 GOPS;
# glmark2-es2-drm --off-screen 1353 through glvnd.
# Needs nixpkgs.config.allowUnfree (libmali is unfree but redistributable).
{ pkgs, ... }:
let
  libmali = pkgs.callPackage ../pkgs/libmali.nix { };
  libmali-glvnd = pkgs.callPackage ../pkgs/libmali-glvnd.nix { inherit libmali; };
  # Only GBM needs swapping; EGL/GLES already reach libmali through glvnd.
  libmali-gbm = pkgs.runCommand "libmali-gbm" { } ''
    mkdir -p $out/lib
    ln -s ${libmali}/lib/mali/libgbm.so.1 $out/lib/libgbm.so.1
  '';
  libmali-run = pkgs.writeShellScriptBin "libmali-run" ''
    export LD_LIBRARY_PATH=${libmali-gbm}/lib''${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
    exec "$@"
  '';
in
{
  hardware.graphics.enable = true;
  # /run/opengl-driver/{etc/OpenCL/vendors/mali.icd, share/vulkan/icd.d/mali.json,
  # share/glvnd/egl_vendor.d/10_mali.json}
  hardware.graphics.extraPackages = [ libmali libmali-glvnd ];

  environment.systemPackages = [
    libmali-run
    pkgs.clinfo
    pkgs.vulkan-tools
  ];
}

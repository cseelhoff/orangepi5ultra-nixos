# Mali-G610 acceleration on the vendor kernel through Arm's closed libmali
# (Mesa's panfrost/panthor do not talk to the vendor kbase driver).
#   OpenCL 3.0 and Vulkan 1.3: system-wide through their ICD files.
#   EGL / GLES 3.2 / GBM: libmali has no glvnd entry point, so run GL programs
#   with `libmali-run <program>` (puts the Mali EGL/GLES/GBM libraries first).
# Tested 2026-10-18 (clpeak): fp32 492 GFLOPS, fp16 964, int8 dot 1934 GOPS.
# Needs nixpkgs.config.allowUnfree (libmali is unfree but redistributable).
{ pkgs, ... }:
let
  libmali = pkgs.callPackage ../pkgs/libmali.nix { };
  libmali-run = pkgs.writeShellScriptBin "libmali-run" ''
    export LD_LIBRARY_PATH=${libmali}/lib/mali:${libmali}/lib''${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
    exec "$@"
  '';
in
{
  hardware.graphics.enable = true;
  # Provides /run/opengl-driver/etc/OpenCL/vendors/mali.icd and
  # /run/opengl-driver/share/vulkan/icd.d/mali.json.
  hardware.graphics.extraPackages = [ libmali ];

  environment.systemPackages = [
    libmali-run
    pkgs.clinfo
    pkgs.vulkan-tools
  ];
}

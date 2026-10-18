# RK3588 NPU (3 cores, vendor RKNPU 0.9.8 driver) through Rockchip's native
# runtime librknnrt 2.3.2. Tested 2026-10-18 with rknn_benchmark:
#   MobileNet v1: 470 inf/s on one core, 769 on all three (one model),
#                 909 total with three processes (one per core)
#   YOLOv5s 640:  60 fps one core, 91 all three, 134 total with three processes
# (Mesa Teflon on mainline managed 69-80 inf/s per process, 247 total.)
# Usage: rknn_benchmark <model.rknn> "" <loops> <core_mask 1|2|4|7>
# Models: /run/current-system/sw/share/rknpu2/models
# Needs nixpkgs.config.allowUnfree (librknnrt is unfree but redistributable).
{ pkgs, ... }:
let
  rknpu2 = pkgs.callPackage ../pkgs/rknpu2.nix { };
in
{
  environment.systemPackages = [ rknpu2 ];
  environment.pathsToLink = [ "/share/rknpu2" ];
}

# Makes Arm's libmali a regular NixOS graphics driver:
#   libEGL_mali.so.0  libglvnd EGL vendor (share/glvnd/egl_vendor.d/10_mali.json),
#                     so unmodified EGL/GLES programs use the Mali GPU.
#   gbm/rockchip_gbm.so  Mesa GBM backend (Mesa loads <drm driver>_gbm.so), so
#                     programs that drive the display through the system libgbm
#                     (compositors, kmscube, glmark2-es2-drm) get buffers Mali's
#                     EGL can render into.
# Installed through hardware.graphics.extraPackages, i.e. /run/opengl-driver.
{ stdenv, libglvnd, libmali }:

stdenv.mkDerivation {
  pname = "libmali-glvnd";
  version = "0.2";
  src = ../tools/libmali-glvnd;
  buildInputs = [ libglvnd ];

  buildPhase = ''
    mali=(-DLIBMALI_PATH="\"${libmali}/lib/libmali.so.1\""
          -DLIBMALI_HOOK_PATH="\"${libmali}/lib/libmali-hook.so.1\"")
    $CC -O2 -Wall -Wextra -fvisibility=hidden -shared -fPIC -o rockchip_gbm.so mali_gbm_backend.c \
      -I. "''${mali[@]}" -ldl -lpthread
    $CC -O2 -Wall -Wextra -shared -fPIC -o libEGL_mali.so.0 egl_mali_glvnd.c \
      "''${mali[@]}" -DMALI_GBM_BACKEND_PATH="\"$out/lib/gbm/rockchip_gbm.so\"" -ldl
  '';

  # 10_ sorts before Mesa's 50_mesa.json, so Mali is asked first for displays.
  installPhase = ''
    install -Dm755 libEGL_mali.so.0 $out/lib/libEGL_mali.so.0
    install -Dm755 rockchip_gbm.so $out/lib/gbm/rockchip_gbm.so
    mkdir -p $out/share/glvnd/egl_vendor.d
    cat > $out/share/glvnd/egl_vendor.d/10_mali.json <<EOF
    {
      "file_format_version" : "1.0.0",
      "ICD" : { "library_path" : "$out/lib/libEGL_mali.so.0" }
    }
    EOF
  '';

  meta.platforms = [ "aarch64-linux" ];
}

# libEGL_mali: registers Arm's libmali with libglvnd as an EGL vendor, so
# unmodified EGL/GLES programs use the Mali GPU (no LD_LIBRARY_PATH wrapper).
# Installed through hardware.graphics.extraPackages, which puts the JSON file
# in /run/opengl-driver/share/glvnd/egl_vendor.d where NixOS's glvnd looks.
{ stdenv, libglvnd, libmali }:

stdenv.mkDerivation {
  pname = "libmali-glvnd";
  version = "0.1";
  src = ../tools/libmali-glvnd;
  buildInputs = [ libglvnd ];

  buildPhase = ''
    $CC -O2 -Wall -shared -fPIC -o libEGL_mali.so.0 egl_mali_glvnd.c \
      -DLIBMALI_PATH='"${libmali}/lib/libmali.so.1"' \
      -DLIBMALI_HOOK_PATH='"${libmali}/lib/libmali-hook.so.1"' -ldl
  '';

  # 10_ sorts before Mesa's 50_mesa.json, so Mali is asked first for displays.
  installPhase = ''
    install -Dm755 libEGL_mali.so.0 $out/lib/libEGL_mali.so.0
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

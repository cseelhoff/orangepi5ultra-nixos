# Rockchip's NPU runtime (librknnrt, closed) for the vendor kernel's RKNPU
# driver, plus Rockchip's rknn_benchmark tool and two prebuilt RK3588 models
# (MobileNet v1, YOLOv5s 640) under share/rknpu2/models.
{ lib, stdenv, fetchFromGitHub, autoPatchelfHook, zlib }:

stdenv.mkDerivation {
  pname = "rknpu2";
  version = "2.3.2";

  src = fetchFromGitHub {
    owner = "airockchip";
    repo = "rknn-toolkit2";
    rev = "2c723177a94fac65cbc036d722907ddbc91980cf"; # master, release v2.3.2
    nonConeMode = true;
    sparseCheckout = [
      "/rknpu2/runtime/Linux/librknn_api/aarch64/librknnrt.so"
      "/rknpu2/runtime/Linux/librknn_api/include/"
      "/rknpu2/examples/rknn_benchmark/src/"
      "/rknpu2/examples/3rdparty/"
      "/rknpu2/examples/rknn_api_demo/model/RK3588/mobilenet_v1.rknn"
      "/rknpu2/examples/rknn_yolov5_demo/model/RK3588/yolov5s-640-640.rknn"
      "/LICENSE"
    ];
    hash = "sha256-T/KqZhTVutp/PdLt/A9N5ajsjKY6e/zZujgCuNl/aAc=";
  };

  nativeBuildInputs = [ autoPatchelfHook ];
  buildInputs = [ stdenv.cc.cc.lib zlib ];

  buildPhase = ''
    runHook preBuild
    api=rknpu2/runtime/Linux/librknn_api
    bench=rknpu2/examples/rknn_benchmark
    $CXX -O2 -std=c++14 -o rknn_benchmark \
      $bench/src/rknn_benchmark.cpp $bench/src/cnpy/cnpy.cpp \
      -I$api/include -Irknpu2/examples/3rdparty \
      -L$api/aarch64 -lrknnrt -lz
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    api=rknpu2/runtime/Linux/librknn_api
    install -Dm755 $api/aarch64/librknnrt.so $out/lib/librknnrt.so
    install -Dm644 -t $out/include $api/include/*.h
    install -Dm755 rknn_benchmark $out/bin/rknn_benchmark
    install -Dm644 rknpu2/examples/rknn_api_demo/model/RK3588/mobilenet_v1.rknn \
      $out/share/rknpu2/models/mobilenet_v1.rknn
    install -Dm644 rknpu2/examples/rknn_yolov5_demo/model/RK3588/yolov5s-640-640.rknn \
      $out/share/rknpu2/models/yolov5s-640-640.rknn
    runHook postInstall
  '';

  meta = {
    description = "Rockchip RKNN NPU runtime (librknnrt) with rknn_benchmark";
    homepage = "https://github.com/airockchip/rknn-toolkit2";
    license = lib.licenses.unfreeRedistributable;
    platforms = [ "aarch64-linux" ];
  };
}

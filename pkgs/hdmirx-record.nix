# hdmirx-record <h264|h265> <seconds> <out.mkv> [bitrate]
# Records HDMI-in with the hardware encoder. The receiver's /dev/videoN changes
# between boots, so it is looked up by name.
{ writeShellApplication, gst_all_1, gst-rockchip, coreutils, gnugrep }:

writeShellApplication {
  name = "hdmirx-record";
  runtimeInputs = [ gst_all_1.gstreamer coreutils gnugrep ];
  runtimeEnv.GST_PLUGIN_SYSTEM_PATH_1_0 = builtins.concatStringsSep ":" (map (p: "${p}/lib/gstreamer-1.0") [
    gst_all_1.gstreamer.out
    gst_all_1.gst-plugins-base
    gst_all_1.gst-plugins-good
    gst_all_1.gst-plugins-bad
    gst-rockchip
  ]);
  text = ''
    codec=''${1:?usage: hdmirx-record <h264|h265> <seconds> <out.mkv> [bitrate]}
    secs=''${2:?seconds}
    out=''${3:?output file}
    bps=''${4:-40000000}
    case $codec in h264|h265) ;; *) echo "codec must be h264 or h265" >&2; exit 1 ;; esac

    name=$(grep -il hdmirx /sys/class/video4linux/*/name | head -n1)
    [ -n "$name" ] || { echo "no HDMI-in video device found" >&2; exit 1; }
    dev=/dev/$(basename "$(dirname "$name")")

    # -e turns the SIGINT below into an end-of-stream, so the file is finalized.
    gst-launch-1.0 -e \
      v4l2src device="$dev" io-mode=dmabuf ! video/x-raw,format=NV12 ! \
      queue ! "mpp''${codec}enc" bps="$bps" rc-mode=cbr ! "''${codec}parse" ! \
      matroskamux ! filesink location="$out" &
    pid=$!
    sleep "$secs"
    kill -INT "$pid"
    wait "$pid"
  '';
}

# hdmirx-audio-stream <rtp://host:port | srt://host:port> [bitrate]
# Streams the HDMI-in audio over the network as Opus (48 kHz stereo).
#   rtp://  RTP over UDP: lowest latency; receive with
#           gst-launch-1.0 udpsrc port=PORT caps="application/x-rtp,media=audio,encoding-name=OPUS,payload=96,clock-rate=48000" ! rtpopusdepay ! opusdec ! autoaudiosink
#   srt://  SRT caller (MPEG-TS inside): retransmits lost packets; receive with
#           ffplay srt://0.0.0.0:PORT?mode=listener
# The RK3588 has no audio encoder; Opus on the CPU costs ~5-9% of one core
# (about 1% of the whole CPU), measured 2026-10-18.
{ writeShellApplication, gst_all_1, coreutils, gnugrep }:

writeShellApplication {
  name = "hdmirx-audio-stream";
  runtimeInputs = [ gst_all_1.gstreamer coreutils gnugrep ];
  runtimeEnv.GST_PLUGIN_SYSTEM_PATH_1_0 = builtins.concatStringsSep ":" (map (p: "${p}/lib/gstreamer-1.0") [
    gst_all_1.gstreamer.out
    gst_all_1.gst-plugins-base
    gst_all_1.gst-plugins-good
    gst_all_1.gst-plugins-bad
  ]);
  text = ''
    dest=''${1:?usage: hdmirx-audio-stream <rtp://host:port|srt://host:port> [bitrate]}
    bps=''${2:-128000}
    grep -qx rockchiphdmiin /proc/asound/card*/id 2>/dev/null || { echo "no HDMI-in sound card" >&2; exit 1; }

    src=(alsasrc "device=hw:CARD=rockchiphdmiin,DEV=0" ! "audio/x-raw,rate=48000,channels=2" !
         queue ! audioconvert ! opusenc "bitrate=$bps" "frame-size=10")
    hostport=''${dest#*://}
    host=''${hostport%:*}
    port=''${hostport##*:}
    case $dest in
      rtp://*) exec gst-launch-1.0 -e "''${src[@]}" ! rtpopuspay ! udpsink "host=$host" "port=$port" ;;
      srt://*) exec gst-launch-1.0 -e "''${src[@]}" ! mpegtsmux ! srtsink "uri=srt://$host:$port" latency=120 ;;
      *) echo "destination must be rtp://host:port or srt://host:port" >&2; exit 1 ;;
    esac
  '';
}

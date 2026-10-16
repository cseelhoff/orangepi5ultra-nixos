#!/usr/bin/env bash
# Orange Pi 5 Ultra benchmark: same tests on any OS, results to one file.
#   ./bench.sh            -> results-<os>-<date>.txt next to this script
#   IPERF_SERVER=host ./bench.sh   also runs iperf3 against that LAN host
# Needs: ffmpeg, fio, stress-ng, v4l2-ctl; optional glmark2*, vulkaninfo,
# eglinfo/es2_info. Missing tools are reported, not fatal. No root needed,
# nothing outside this directory is written.
set -u
cd "$(dirname "$0")"
OS=$(. /etc/os-release; echo "${ID}")
OUT="$PWD/results-${OS}-$(date +%Y%m%d-%H%M).txt"
WORK="$PWD/work"; mkdir -p "$WORK"
exec > >(tee "$OUT") 2>&1

sec()  { printf '\n==================== %s ====================\n' "$*"; }
have() { command -v "$1" >/dev/null 2>&1; }
run()  { echo "\$ $*"; timeout "${T:-300}" "$@" 2>&1; echo "[exit $?]"; }
temps(){ for z in /sys/class/thermal/thermal_zone*; do printf '%s=%s ' "$(cat $z/type)" "$(( $(cat $z/temp) / 1000 ))C"; done; echo; }

sec "System"
date; uname -a; grep PRETTY_NAME /etc/os-release
tr -d '\0' < /proc/device-tree/model; echo
nproc; free -h | head -2
for p in /sys/devices/system/cpu/cpufreq/policy*; do echo "$(basename $p): $(cat $p/scaling_governor) $(( $(cat $p/cpuinfo_max_freq) / 1000 )) MHz max"; done
echo "temps: $(temps)"

sec "Device nodes and drivers"
ls -l /dev/mali0 /dev/mpp_service /dev/rga /dev/dri/ /dev/video* 2>&1
for c in /sys/class/drm/card* ; do [ -e $c/device/driver ] && echo "$(basename $c) -> $(basename $(readlink -f $c/device/driver))"; done | sort -u
lsmod 2>/dev/null | grep -iE '^(panthor|panfrost|mali|bifrost|rknpu)' || true
dmesg 2>/dev/null | grep -iE 'rknpu.*version|mali .*DDK|panthor' | head -5 || echo "(dmesg not readable)"

sec "GPU: EGL / GLES / Vulkan driver in use"
have eglinfo   && T=30 run eglinfo -B 2>&1 | grep -iE 'vendor|renderer|version' | head -12
have es2_info  && T=30 run es2_info 2>&1 | grep -iE 'GL_RENDERER|GL_VERSION|GL_VENDOR'
have vulkaninfo && T=30 run vulkaninfo --summary 2>&1 | grep -iE 'deviceName|driverName|driverInfo|apiVersion|ERROR' | head -10

sec "GPU: glmark2"
# drm needs no running desktop; wayland/x11 need one. First that runs wins.
for g in glmark2-es2-drm glmark2-es2-wayland glmark2-es2 glmark2; do
  if have $g; then T=600 run $g --fullscreen 2>&1 | grep -E 'GL_RENDERER|GL_VERSION|^\[|glmark2 Score|Error|error' | tail -40
    grep -q 'glmark2 Score' "$OUT" && break; fi
done

sec "CPU (stress-ng, 30 s each)"
echo "temps before: $(temps)"
have stress-ng && { T=60 run stress-ng --cpu 1 --cpu-method matrixprod -t 30s --metrics-brief
                    T=60 run stress-ng --cpu "$(nproc)" --cpu-method matrixprod -t 30s --metrics-brief
                    T=60 run stress-ng --memcpy 4 -t 20s --metrics-brief; }
echo "temps after: $(temps)"
grep -H . /sys/devices/system/cpu/cpufreq/policy*/scaling_cur_freq 2>/dev/null

sec "Storage (fio on the disk holding this directory, 512 MiB file)"
df -h "$PWD" | tail -1
if have fio; then
  for j in "seqread read 1M" "seqwrite write 1M" "randread randread 4k" "randwrite randwrite 4k"; do
    set -- $j
    T=120 run fio --name="$1" --filename="$WORK/fio.dat" --size=512M --rw="$2" --bs="$3" \
      --direct=1 --ioengine=libaio --iodepth=32 --runtime=20 --time_based --group_reporting \
      | grep -E 'IOPS=|lat \(usec\): min|ERROR'
  done; rm -f "$WORK/fio.dat"
fi

sec "Video: test clips"
have ffmpeg || echo "ffmpeg MISSING"
ffmpeg -hide_banner -decoders 2>/dev/null | grep -iE 'rkmpp|v4l2m2m' || echo "(no rkmpp/v4l2m2m decoders in this ffmpeg)"
ffmpeg -hide_banner -encoders 2>/dev/null | grep -iE 'rkmpp|v4l2m2m' || echo "(no rkmpp/v4l2m2m encoders in this ffmpeg)"
for r in 1920x1080 3840x2160; do
  [ -s "$WORK/clip-$r.mp4" ] || T=600 run ffmpeg -hide_banner -loglevel error -y -f lavfi \
    -i "testsrc2=size=$r:rate=30:duration=10" -c:v libx264 -preset ultrafast -pix_fmt yuv420p "$WORK/clip-$r.mp4"
done

sec "Video: decode speed (H.264, 300 frames)"
for r in 1920x1080 3840x2160; do
  echo "--- $r software"
  T=300 run ffmpeg -hide_banner -benchmark -i "$WORK/clip-$r.mp4" -f null - 2>&1 | grep -E 'frame=.*speed|bench:' | tail -2
  if ffmpeg -hide_banner -decoders 2>/dev/null | grep -q h264_rkmpp; then
    echo "--- $r h264_rkmpp (hardware)"
    T=300 run ffmpeg -hide_banner -benchmark -c:v h264_rkmpp -i "$WORK/clip-$r.mp4" -f null - 2>&1 | grep -E 'frame=.*speed|bench:|rror' | tail -3
  fi
done

sec "Video: encode speed (1080p H.264, 300 frames)"
T=300 run ffmpeg -hide_banner -benchmark -f lavfi -i "testsrc2=size=1920x1080:rate=30:duration=10" \
  -c:v libx264 -preset veryfast -f null - 2>&1 | grep -E 'frame=.*speed|bench:' | tail -2
if ffmpeg -hide_banner -encoders 2>/dev/null | grep -q h264_rkmpp; then
  echo "--- h264_rkmpp (hardware)"
  T=300 run ffmpeg -hide_banner -benchmark -f lavfi -i "testsrc2=size=1920x1080:rate=30:duration=10" \
    -c:v h264_rkmpp -f null - 2>&1 | grep -E 'frame=.*speed|bench:|rror' | tail -3
fi

sec "HDMI capture (hdmirx)"
DEV=""
for v in /sys/class/video4linux/video*; do grep -qi hdmirx "$v/name" 2>/dev/null && DEV=/dev/$(basename $v); done
echo "hdmirx device: ${DEV:-none found}"
if [ -n "$DEV" ] && have v4l2-ctl; then
  T=10 run v4l2-ctl -d "$DEV" --query-dv-timings | grep -E 'Active|frames per second'
  T=10 run v4l2-ctl -d "$DEV" --get-fmt-video
  T=60 run v4l2-ctl -d "$DEV" --stream-mmap=4 --stream-count=600 --stream-to=/dev/null 2>&1 | tr '\r' '\n' | grep -E 'fps' | tail -3
  T=20 run v4l2-ctl -d "$DEV" --stream-mmap --stream-count=1 --stream-to="$WORK/hdmi-frame.raw"
  ls -l "$WORK/hdmi-frame.raw" 2>/dev/null
fi

sec "Network"
have nmcli && nmcli -f IN-USE,SSID,CHAN,RATE,SIGNAL,SECURITY dev wifi 2>/dev/null | grep -E '^\*|IN-USE'
ip -br a | grep -v '^lo'
for i in /sys/class/net/e*; do [ -e "$i" ] && echo "$(basename $i) speed: $(cat $i/speed 2>/dev/null) Mb/s"; done
if [ -n "${IPERF_SERVER:-}" ] && have iperf3; then
  T=40 run iperf3 -c "$IPERF_SERVER" -t 10
  T=40 run iperf3 -c "$IPERF_SERVER" -t 10 -R
else echo "(set IPERF_SERVER=<LAN host running 'iperf3 -s'> to test throughput)"; fi

sec "Done"
echo "temps: $(temps)"
echo "results: $OUT"

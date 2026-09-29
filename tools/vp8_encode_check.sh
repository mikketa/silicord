#!/bin/sh
# Re-encodes some of the VP8 test vectors' pictures with our encoder, then checks that ffmpeg's decoder
# (independent of ours) reads the streams back to exactly what our decoder does.
#   tools/vp8_encode_check.sh path/to/vp8_roundtrip dir-with-the-vectors
set -e
tool=$1
dir=$2
status=0
for i in 001 004 008 014 017; do
    for kbps in 150 600; do
        src="$dir/vp80-00-comprehensive-$i.ivf"
        out="$dir/ours-$i-$kbps"
        "$tool" "$src" $kbps "$out.ivf" "$out.yuv" || status=1
        ffmpeg -v error -y -i "$out.ivf" -f rawvideo -pix_fmt yuv420p "$out.ffmpeg.yuv"
        if cmp -s "$out.yuv" "$out.ffmpeg.yuv"; then
            echo "$out.ivf: ffmpeg decodes it identically"
        else
            echo "$out.ivf: ffmpeg decodes it differently"
            status=1
        fi
    done
done
"$tool" --speed 640 360 30
exit $status

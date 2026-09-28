#!/bin/sh
# Decodes libvpx's eighteen VP8 "comprehensive" test vectors and checks every frame's MD5.
#   tools/vp8_vectors.sh path/to/vp8_vectors dir-for-the-vectors
set -e
tool=$1
dir=$2
base=https://storage.googleapis.com/downloads.webmproject.org/test_data/libvpx
mkdir -p "$dir"
status=0
for i in 001 002 003 004 005 006 007 008 009 010 011 012 013 014 015 016 017 018; do
    f=vp80-00-comprehensive-$i.ivf
    for e in "" .md5; do
        [ -f "$dir/$f$e" ] || curl -sSfLo "$dir/$f$e" "$base/$f$e"
    done
    "$tool" "$dir/$f" "$dir/$f.md5" || status=1
done
exit $status

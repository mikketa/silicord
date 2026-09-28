#!/bin/sh
# Decodes the twelve Opus test vectors of RFC 8251 and checks them against the reference decodings.
#   tools/opus_vectors.sh path/to/opus_vectors dir-for-the-vectors
set -e
tool=$1
dir=$2
if [ ! -f "$dir/testvector12.dec" ]; then
    mkdir -p "$dir"
    curl -sSfL https://opus-codec.org/static/testvectors/opus_testvectors-rfc8251.tar.gz | tar xz -C "$dir" --strip-components=1
fi
status=0
for i in 01 02 03 04 05 06 07 08 09 10 11 12; do
    "$tool" "$dir/testvector$i.bit" "$dir/testvector$i.dec" || status=1
done
exit $status

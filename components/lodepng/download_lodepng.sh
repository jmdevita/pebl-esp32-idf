#!/bin/bash
# Download lodepng single-file PNG decoder library.
# Run once after cloning the repo:
#   cd components/lodepng && ./download_lodepng.sh
#
# lodepng is a single-file C library by Lode Vandevenne.
# Source: https://github.com/lvandeve/lodepng
# License: zlib/libpng (very permissive, compatible with any project)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

echo "Downloading lodepng..."
curl -sL "https://raw.githubusercontent.com/lvandeve/lodepng/master/lodepng.h" -o lodepng.h
curl -sL "https://raw.githubusercontent.com/lvandeve/lodepng/master/lodepng.cpp" -o lodepng.c

echo "lodepng downloaded successfully."
echo "  lodepng.h  ($(wc -c < lodepng.h) bytes)"
echo "  lodepng.c  ($(wc -c < lodepng.c) bytes)"

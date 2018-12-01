#! /bin/sh
set -e

BUILD_DIR="$1"
CEF_DIR="$2"
CMAKE="$3"
OUTPUT="$4"
STAMP="$5"

! [ -d "$BUILD_DIR" ] || rm -r "$BUILD_DIR"
mkdir "$BUILD_DIR"
( cd "$BUILD_DIR" && $CMAKE -G Ninja "$CEF_DIR" && ninja libcef_dll_wrapper )
cp "$BUILD_DIR"/libcef_dll_wrapper/libcef_dll_wrapper.a "$OUTPUT"
touch "$STAMP"

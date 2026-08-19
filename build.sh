#!/bin/sh
# 一键构建（无 WiFi 版，默认 <200KB）
set -e

. ~/esp/esp-idf/export.sh

idf.py build
idf.py size
idf.py merge-bin -o xiaomiao-loader-merged.bin

echo ""
echo "Done."
echo "  build/xiaomiao-loader-baremetal.bin   (app)"
echo "  xiaomiao-loader-merged.bin            (full flash image)"
echo ""
echo "For WiFi version:"
echo "  idf.py menuconfig → Component config → Xiaomiao Loader → Enable WiFi OTA"
echo "  idf.py build"
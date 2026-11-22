#!/bin/sh
# Cross-build build/librmpp-textobj-ui.so for the reMarkable Paper Pro (aarch64, Qt 6). SDK defaults to ../sdk.
set -e
cd "$(dirname "$0")"
SDK=${SDK:-../sdk}
CXX=${CXX:-$SDK/bin/aarch64-none-linux-gnu-g++}
Q=$SDK/qt/usr/include/qt6
mkdir -p build
"$CXX" -std=c++17 -O2 -fPIC -shared -fvisibility=hidden -DQT_NO_VERSION_TAGGING -DQT_NO_DEBUG \
  -I"$Q" -I"$Q/QtCore" -I"$Q/QtGui" -I"$Q/QtQml" -I"$Q/QtQuick" \
  src/textobj.cpp -o build/librmpp-textobj-ui.so -L"$SDK/devlibs" \
  -l:libQt6Core.so.6 -l:libQt6Gui.so.6 -l:libQt6Qml.so.6 -l:libQt6Quick.so.6 \
  -ldl -Wl,--allow-shlib-undefined -Wl,-soname,librmpp-textobj-ui.so
ls -la build/librmpp-textobj-ui.so

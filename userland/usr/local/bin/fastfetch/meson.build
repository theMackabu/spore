userland_fastfetch = custom_target('userland-fastfetch',
  output: 'fastfetch',
  command: [
    'sh', '-c',
    'set -eu; src="$1"; build="$2"; out="$3"; jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"; cmake -S "$src" -B "$build" -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64 -DCMAKE_C_COMPILER=aarch64-unknown-linux-musl-gcc -DCMAKE_BUILD_TYPE=MinSizeRel -DCMAKE_EXE_LINKER_FLAGS=-static -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY -DIS_MUSL=ON -DBUILD_FLASHFETCH=OFF -DBUILD_TESTS=OFF -DENABLE_THREADS=OFF -DENABLE_VULKAN=OFF -DENABLE_WAYLAND=OFF -DENABLE_XCB_RANDR=OFF -DENABLE_XRANDR=OFF -DENABLE_DRM=OFF -DENABLE_DRM_AMDGPU=OFF -DENABLE_GIO=OFF -DENABLE_DCONF=OFF -DENABLE_EET=OFF -DENABLE_DBUS=OFF -DENABLE_SQLITE3=OFF -DENABLE_RPM=OFF -DENABLE_IMAGEMAGICK7=OFF -DENABLE_IMAGEMAGICK6=OFF -DENABLE_CHAFA=OFF -DENABLE_EGL=OFF -DENABLE_GLX=OFF -DENABLE_OPENCL=OFF -DENABLE_FREETYPE=OFF -DENABLE_PULSE=OFF -DENABLE_DDCUTIL=OFF -DENABLE_ELF=OFF -DENABLE_ZLIB=OFF -DENABLE_LUA=OFF -DENABLE_QUICKJS=OFF -DENABLE_LIBZFS=OFF -DENABLE_WORDEXP=OFF -DENABLE_SYSTEM_YYJSON=OFF >/dev/null; cmake --build "$build" --target fastfetch -j "$jobs"; aarch64-unknown-linux-musl-strip -o "$out" "$build/fastfetch"',
    'sh',
    meson.project_source_root() / 'userland/third_party/fastfetch',
    meson.current_build_dir() / 'fastfetch-build',
    '@OUTPUT@',
  ])
userland_image_targets += userland_fastfetch

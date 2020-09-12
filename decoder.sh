#! /bin/bash

apt-get update
apt-get install -y pkg-config

# git clone --depth 1 --branch n4.0.2 https://github.com/FFmpeg/FFmpeg
git clone --depth 1 https://github.com/FFmpeg/FFmpeg

pushd FFmpeg

emconfigure ./configure \
  --cc=emcc \
  --cxx=em++ \
  --objcc=emcc \
  --dep-cc=emcc \
  --nm="llvm-nm -g" \
  --ranlib=emranlib \
  --enable-cross-compile \
  --target-os=none \
  --arch=x86 \
  --disable-runtime-cpudetect \
  --disable-asm \
  --disable-fast-unaligned \
  --disable-w32threads \
  --disable-os2threads \
  --disable-debug \
  --disable-stripping \
  --disable-doc \
  --disable-safe-bitstream-reader \
  \
  --disable-all \
  --enable-decoders \
  --enable-demuxers \
  --enable-avcodec \
  --enable-avformat \
  --disable-network \
  --disable-d3d11va \
  --disable-dxva2 \
  --disable-vaapi \
  --disable-vdpau \
  --enable-protocol=file \
  --disable-bzlib \
  --disable-iconv \
  --disable-libxcb \
  --disable-lzma \
  --disable-sdl2 \
  --disable-xlib

emmake make -j4

emcc -Oz \
  -I. \
  -Llibavcodec -Llibavformat -Llibavutil -lavformat -lavcodec -lavutil \
  -o ../src/decoder.js ../src/decoder.c \
  -s USE_PTHREADS=1 \
  -s ALLOW_MEMORY_GROWTH=1 \
  -s EXPORTED_FUNCTIONS='["_decoder"]' \
  -s EXPORTED_RUNTIME_METHODS='["cwrap"]' \
  --post-js ../src/post.js

popd

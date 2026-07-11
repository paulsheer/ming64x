#!/bin/sh
# Reconfigure libX11 for MinGW-w64 cross-compilation (64-bit Windows)
cd "$(dirname "$0")"
PREFIX=/paulsbackup/desktop-10.1.0.7/root/cooledit/git/deepseek-workspace/dep-vcxsrv/prefix
INCDIR=$PREFIX/include
./configure \
  --host=x86_64-w64-mingw32 \
  --prefix=$PREFIX \
  --enable-static --disable-shared \
  --disable-xf86bigfont \
  --disable-ipv6 \
  CFLAGS="-I$INCDIR -I/paulsbackup/desktop-10.1.0.7/root/cooledit/git/deepseek-workspace/dep-vcxsrv/xorgproto/include" \
  XAU_CFLAGS=-I$INCDIR \
  XAU_LIBS="-L$PREFIX/lib -lXau" \
  XDMCP_CFLAGS=-I$INCDIR \
  XDMCP_LIBS="-L$PREFIX/lib -lXdmcp" \
  XCB_CFLAGS=-I$INCDIR \
  XCB_LIBS="-L$PREFIX/lib -lxcb -lXau -lXdmcp" \
  ac_cv_func_malloc_0_nonnull=yes \
  ac_cv_func_realloc_0_nonnull=yes \
  xorg_cv_malloc0_returns_null=no

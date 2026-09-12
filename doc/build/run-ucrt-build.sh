#!/bin/bash
export PATH="/d/msys64/ucrt64/bin:/d/msys64/usr/bin:/c/Users/ChaosMugen/.cargo/bin:/c/Ruby40-x64/bin:$PATH"
export MSYSTEM=UCRT64
export ac_cv_prog_cc_x86_64_w64_mingw32_cc__UCRT=yes
cd /d/ruby-build/ruby40-ucrt
autoconf || exit 1
CC=gcc ./configure \
  \
  --prefix=D:/ruby-build/ruby40-ucrt-install \
  --disable-install-doc \
  --disable-install-rdoc \
  --with-baseruby=C:/Ruby40-x64/bin/ruby.exe \
  --enable-yjit \
  CFLAGS="-O2 -g" \
  > /d/ruby-build/conf40ucrt.log 2>&1
CONF_RC=$?
make -j8 > /d/ruby-build/make40ucrt.log 2>&1
MAKE_RC=$?
echo "BUILD-DONE conf_rc=$CONF_RC make_rc=$MAKE_RC" >> /d/ruby-build/make40ucrt.log

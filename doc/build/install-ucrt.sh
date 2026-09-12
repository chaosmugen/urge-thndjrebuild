#!/bin/bash
# Install the mingw-ucrt build to --prefix (D:/ruby-build/ruby40-ucrt-install).
export MSYSTEM=UCRT64
export PATH="/d/msys64/ucrt64/bin:/d/msys64/usr/bin:/c/Users/ChaosMugen/.cargo/bin:$PATH"
cd /d/ruby-build/ruby40-ucrt || exit 1
make install > /d/ruby-build/install40ucrt.log 2>&1
echo "INSTALL_EXIT=$?" >> /d/ruby-build/install40ucrt.log

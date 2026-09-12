#!/bin/bash
# Build the prepared mingw-ucrt tree.
# Where the earlier fixes live now:
#  - rust std needs ntdll/userenv/advapi32: patched into rbconfig.rb (MAINLIBS)
#    and the generated Makefile (EXTLIBS/MAINLIBS), so extension link tests and
#    every link step pick them up without command-line overrides.
#  - target/release/libyjit.localized.a must NOT be pre-created; the make rule
#    builds it with the objcopy localization that avoids lgamma_r collisions.
export MSYSTEM=UCRT64
export PATH="/d/msys64/ucrt64/bin:/d/msys64/usr/bin:/c/Users/ChaosMugen/.cargo/bin:$PATH"
cd /d/ruby-build/ruby40-ucrt || exit 1

make -j8 > /d/ruby-build/make40ucrt2.log 2>&1
echo "MAKE_EXIT=$?" >> /d/ruby-build/make40ucrt2.log

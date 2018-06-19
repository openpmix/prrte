#!/bin/sh
set -ex
git clone --depth=1 -b topic/pmix_setup_cc https://github.com/ggouaillardet/pmix.git pmix-master
cd pmix-master
which gcc
type gcc
ls -l /usr/bin/gcc
ls -l /usr/bin/gcc-4.8 || true
gcc --version
./autogen.pl && ./configure --prefix=/usr && cat src/include/pmix_config.h && make V=1 && sudo make install

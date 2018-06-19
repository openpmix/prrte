#!/bin/sh
set -ex
git clone --depth=1 https://github.com/pmix/pmix pmix-master
cd pmix-master
which gcc
type gcc
alias gcc
gcc --version
./autogen.pl && ./configure --prefix=/usr && cat src/include/pmix_config.h && make V=1 && sudo make install

#!/bin/sh
set -ex
git clone --depth=1 https://github.com/pmix/pmix pmix-master
cd pmix-master
./autogen.pl && ./configure --prefix=/usr --disable-picky $* && make && sudo make install

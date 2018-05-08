#!/bin/sh
set -ex
git clone https://github.com/pmix/pmix pmix-master
cd pmix-master
./autogen.pl && ./configure --prefix=/usr && make && sudo make install

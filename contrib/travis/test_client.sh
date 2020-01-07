#!/bin/sh

cd examples
export PATH=$HOME/bogus/bin:$PATH
export LD_LIBRARY_PATH=$HOME/bogus/lib:$HOME/bogus/lib/pmix:/usr/lib:/usr/lib/pmix:$LD_LIBRARY_PATH

make client

set -x

prte --daemonize --host localhost:5

prun -n 2 ./client
ret=$?

prun --terminate

exit $ret

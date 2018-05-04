#!/bin/sh

cd examples
export PATH=$HOME/bogus/bin:$PATH
export LD_LIBRARY_PATH=$HOME/bogus/lib:$HOME/bogus/lib/pmix:/usr/lib:/usr/lib/pmix:$LD_LIBRARY_PATH

make client

prte &
# wait a little for the server to start
sleep 2
prun --oversubscribe -n 2 ./client
ret=$?

prun --terminate

exit $ret

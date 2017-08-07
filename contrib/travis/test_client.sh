#!/bin/sh

cd examples
export PATH=$HOME/bogus/bin:$PATH

make client

psrvr &
prun --oversubscribe -n 2 ./client
ret=$?

prun --terminate

exit $ret

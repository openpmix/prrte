#!/bin/sh

cd examples
export PATH=$HOME/bogus/bin:$PATH

make client

psrvr &
# wait a little for the server to start
sleep 2
prun --oversubscribe -n 2 ./client
ret=$?

prun --terminate

exit $ret

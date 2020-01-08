#!/bin/sh

cd examples
export PATH=$HOME/bogus/bin:$PATH
export LD_LIBRARY_PATH=$HOME/bogus/lib:$HOME/bogus/lib/pmix:/usr/lib:/usr/lib/pmix:$LD_LIBRARY_PATH
export PMIX_MCA_pmix_client_event_verbose=5 PMIX_MCA_pmix_server_event_verbose=5

make client

set -x

prte --host localhost:5 --mca state_base_verbose 10 &

prun -n 2 ./client
ret=$?

prun --terminate

exit $ret

#!/bin/bash

cd /tracemate
export LD_LIBRARY_PATH=$(pwd)
export LD_PRELOAD=/tracemate/libjemalloc.so.2

if [ -f /tracemate/conf/tm.conf.b64 ]; then
    echo "---- tm.conf.b64 contents ----"
    cat /tracemate/conf/tm.conf.b64
    echo "---- tm.conf.b64 contents ----"

    cat /tracemate/conf/tm.conf.b64 | base64 -d > /tracemate/tm.conf
fi

# ensure trace dir exists
mkdir -p /tracemate/data/traces
# ensure transaction store folder exists
mkdir -p /tracemate/data/ts

# clean excess older cores
pushd /tracemate/data/traces > /dev/null
ls -t | sed -e '1,5d' | xargs -d '\n' rm
popd > /dev/null

# now run the actual app
export MTEV_DIAGNOSE_CRASH=1
./tm -c /tracemate/tm.conf -D -D


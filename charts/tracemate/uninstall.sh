#!/bin/bash

set -e

INSTANCE_COUNT=30

for ((i=0; i < $INSTANCE_COUNT; i++)); do
    echo "Instance: $i"
    helm delete --purge tracemate-${i}
done

#!/bin/bash

#set -x

helm repo update

set -e

INSTANCE_COUNT=20
LOOP_COUNT=${1:-$INSTANCE_COUNT}
TOTAL_PARTITION_COUNT=100

for ((i=0; i < $LOOP_COUNT; i++)); do
    echo "Instance: $i"
    # build the partition list
    PARTITION_LIST=""
    p=$((i * (TOTAL_PARTITION_COUNT/INSTANCE_COUNT)))
    max=$((p + (TOTAL_PARTITION_COUNT/INSTANCE_COUNT)))
    for (( ; p < $max; p++)); do
        if [ -n "$PARTITION_LIST" ]; then
            PARTITION_LIST="$PARTITION_LIST,"
        fi
        PARTITION_LIST="${PARTITION_LIST}${p}"
    done
    echo $PARTITION_LIST
    helm upgrade --install \
         tracemate-${i} \
         chartmuseum/tracemate \
         --namespace tracemate \
         --set service.name=tracemate-${i} \
         --set kafkaProperties.kafka_group_id=tracemate_a${i} \
         --set kafkaProperties.kafka_partitions={${PARTITION_LIST}} \
         --set persistence.enabled=true \
         --set persistence.claimName=tracemate-${i}-databig \
         --set app.instance=${i} \
         --set nameOverride=tracemate-${i} \
         --version=0.5.0

done

#!/bin/bash
for i in {1..30}
do
    $@ &
    #pids[${i}]=$!
done

## wait for all pids
#for pid in ${pids[*]}; do
#    wait $pid
#done
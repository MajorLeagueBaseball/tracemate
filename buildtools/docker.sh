#!/bin/bash

cd $(dirname $0); cd ..

while [[ "$#" > 0 ]]
do
    cmd="$1"
    shift
    case $cmd in
        #
        # Create Docker Images
        #
        --build-base)
            echo "Rebuilding tracemate base image"
            docker build . -t tracemate-base:latest -f Dockerfile-base
            ;;
        --build)
            echo "Rebuilding tracemate dev image"
            docker build . -t tracemate:dev -f Dockerfile-dev
            ;;
        --shell)
            echo "Running tracemate container image"
            docker run -h tracemate -it --privileged -p 8888:8888 -v $(pwd):/tracemate --memory 4000G tracemate:dev $@
            exit 0
            ;;
        *)
            echo "Unknown command $1"
            exit 1
            ;;
    esac;
done

exit 0


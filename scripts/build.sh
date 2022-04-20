#!/bin/bash
# This scripts expects seastar-build container to be built e.g. like this
# $ docker build -t seastar-build  -f ./scripts/Dockerfile-build .

BUILDER_IMAGE=${BUILDER_IMAGE:=seastar-build}

if [ $# -eq 0 -o "$1" == "--help" -o "$1" == "-h" ]; then
    echo "Usage: $(basename $0) <mode> [<compiler>] [<version>] [<c++ dialect>]"
    exit 0
fi

if [ -z "$CONTAINERIZED" ]; then
    echo "Wrapping self into $BUILDER_IMAGE container"
    exec docker run -it --rm -v$(pwd):/home/src -e 'CONTAINERIZED=yes' $BUILDER_IMAGE /home/src/scripts/$(basename $0) "$@"
fi

set -e
set -x
update-alternatives --auto gcc
update-alternatives --auto clang

MODE=$1
COMPILER=$2
VERSION=$3
DIALECT=$4

CONFIGURE="--mode=$MODE"
if [ ! -z "$COMPILER" ]; then
    if [ "$COMPILER" == "gcc" ]; then
        CPP_COMPILER="g++"
    elif [ "$COMPILER" == "clang" ]; then
        CPP_COMPILER="clang++"
    else
        echo "Unknown compiler (use 'gcc' or 'clang')"
        exit 1
    fi
    CONFIGURE="$CONFIGURE --compiler=$CPP_COMPILER"

    if [ ! -z "$VERSION" ]; then
        update-alternatives --set $COMPILER /usr/bin/${COMPILER}-${VERSION}
        update-alternatives --set $CPP_COMPILER /usr/bin/${CPP_COMPILER}-${VERSION}

        if [ ! -z "$DIALECT" ]; then
            CONFIGURE="$CONFIGURE --c++-dialect=$DIALECT"
        fi
    fi
fi

cd /home/src
./configure.py $CONFIGURE
ninja -C build/$MODE

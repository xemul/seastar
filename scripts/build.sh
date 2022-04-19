#!/bin/bash
# This scripts expects seastar-build container to be built e.g. like this
# $ docker build -t seastar-build  -f ./scripts/Dockerfile-build .

if [ $# -ne 4 ]; then
    echo "Usage: $(basename $0) <compiler> <version> <mode> <c++ dialect>"
    exit 0
fi

if [ -z "$CONTAINERIZED" ]; then
    echo "Wrapping self into container"
    exec docker run -it --rm -v$(pwd):/home/src -e 'CONTAINERIZED=yes' seastar-build /home/src/scripts/$(basename $0) "$@"
fi

COMPILER=$1
VERSION=$2
MODE=$3
DIALECT=$4

set -e
set -x

if [ "$COMPILER" == "gcc" ]; then
    update-alternatives --set gcc /usr/bin/gcc-$VERSION
    update-alternatives --set g++ /usr/bin/g++-$VERSION
    COMPILER='g++'
elif [ "$COMPILER" == "clang" ]; then
    update-alternatives --set clang /usr/bin/clang-$VERSION
    update-alternatives --set clang++ /usr/bin/clang++-$VERSION
    COMPILER='clang++'
else
    echo "Unknown compiler (use 'gcc' or 'clang')"
    exit 0
fi

cd /home/src
./configure.py --compiler=$COMPILER --mode=$MODE --c++-dialect=$DIALECT
ninja -C build/$MODE

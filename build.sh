#!/bin/sh

mkdir -p build;
touch build/build.md5;
md5sum find-srcs.cpp > build/sources.md5;

if (! cmp --quiet build/build.md5 build/sources.md5); then
    if (c++ find-srcs.cpp -o build/find-srcs); then
        cp build/sources.md5 build/build.md5
    fi
fi

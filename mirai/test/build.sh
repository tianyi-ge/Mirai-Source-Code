#!/bin/bash

SRC_FILES=`find ../bot -name '*.c' -a ! -name 'main.c'`
SRC_FILES=$SRC_FILES" attack_test.c"

if [ $# == 2 ]; then
    if [ "$2" == "telnet" ]; then
        FLAGS="-DMIRAI_TELNET"
    elif [ "$2" == "ssh" ]; then
        FLAGS="-DMIRAI_SSH"
    fi
else
    echo "Missing build type." 
    echo "Usage: $0 <debug | release> <telnet | ssh>"
fi

if [ $# == 0 ]; then
    echo "Usage: $0 <debug | release> <telnet | ssh>"
elif [ "$1" == "release" ]; then
    echo "not supported"
elif [ "$1" == "debug" ]; then
    [ ! -d "debug" ] && mkdir debug
    gcc -std=c99 $SRC_FILES -DDEBUG "$FLAGS" -static -g -o debug/mirai.dbg
    mips-gcc -std=c99 -DDEBUG $SRC_FILES "$FLAGS" -static -g -o debug/mirai.mips
    armv4l-gcc -std=c99 -DDEBUG $SRC_FILES "$FLAGS" -static -g -o debug/mirai.arm
    armv6l-gcc -std=c99 -DDEBUG $SRC_FILES "$FLAGS" -static -g -o debug/mirai.arm7
    sh4-gcc -std=c99 -DDEBUG $SRC_FILES "$FLAGS" -static -g -o debug/mirai.sh4
else
    echo "Unknown parameter $1: $0 <debug | release>"
fi

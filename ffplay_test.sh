#!/usr/bin/env sh

make
./bin/webcamize -x -f | ffplay -probesize 32 -sync ext -fflags nobuffer -

#!/usr/bin/env sh

make
./bin/webcamize | ffplay -probesize 32 -sync ext -fflags nobuffer -

#!/usr/bin/env sh

make
./bin/webcamize -f | ffplay -i -

#!/bin/sh
[ -x engine/build/pagedb ] || ./build.sh
python3 server/server.py "$@"

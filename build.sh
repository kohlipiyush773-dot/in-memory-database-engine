#!/bin/sh
# Linux / macOS build (g++ or clang++)
set -e
mkdir -p engine/build
SRC="engine/src/slotted_page.cpp engine/src/disk_manager.cpp engine/src/log_manager.cpp engine/src/replacers.cpp engine/src/buffer_pool_manager.cpp engine/src/btree.cpp engine/src/sql.cpp engine/src/database.cpp engine/src/executor.cpp engine/src/simulator.cpp"
CXX=${CXX:-g++}
$CXX -std=gnu++14 -O2 -Wall -Iengine/include $SRC engine/src/protocol.cpp engine/src/main.cpp -o engine/build/pagedb
$CXX -std=gnu++14 -O2 -Wall -Iengine/include $SRC engine/tests/test_engine.cpp -o engine/build/test_engine
echo "BUILD OK.  Tests: (cd engine/build && ./test_engine)   App: ./run.sh"

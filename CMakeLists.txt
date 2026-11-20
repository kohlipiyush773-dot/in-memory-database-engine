cmake_minimum_required(VERSION 3.10)
project(PageDB CXX)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
include_directories(engine/include)
add_library(pagedb_core
  engine/src/slotted_page.cpp engine/src/disk_manager.cpp engine/src/log_manager.cpp engine/src/replacers.cpp
  engine/src/buffer_pool_manager.cpp engine/src/btree.cpp engine/src/sql.cpp engine/src/database.cpp
  engine/src/executor.cpp engine/src/simulator.cpp)
add_executable(pagedb engine/src/protocol.cpp engine/src/main.cpp)
target_link_libraries(pagedb pagedb_core)
add_executable(test_engine engine/tests/test_engine.cpp)
target_link_libraries(test_engine pagedb_core)
enable_testing()
add_test(NAME engine_tests COMMAND test_engine)

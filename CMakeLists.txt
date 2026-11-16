cmake_minimum_required(VERSION 3.25)
project(libvsr LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(VSR_ENABLE_ASAN "Build with AddressSanitizer" OFF)
option(VSR_ENABLE_UBSAN "Build with UndefinedBehaviorSanitizer" OFF)
option(VSR_BUILD_TESTS "Build the tests" ON)

add_library(vsr_options INTERFACE)
target_compile_options(vsr_options INTERFACE
  -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror)
if(VSR_ENABLE_ASAN)
  target_compile_options(vsr_options INTERFACE -fsanitize=address -fno-omit-frame-pointer)
  target_link_options(vsr_options INTERFACE -fsanitize=address)
endif()
if(VSR_ENABLE_UBSAN)
  target_compile_options(vsr_options INTERFACE -fsanitize=undefined -fno-sanitize-recover=undefined)
  target_link_options(vsr_options INTERFACE -fsanitize=undefined)
endif()

add_library(vsr
  src/checksum.cpp
  src/client_core.cpp
  src/client_table.cpp
  src/config.cpp
  src/journal.cpp
  src/message.cpp
  src/replica.cpp
  src/superblock.cpp)
target_include_directories(vsr PUBLIC include)
target_link_libraries(vsr PRIVATE vsr_options)

# The default storage, network, client, and runner. Linux only.
add_library(vsr_io
  src/io/client.cpp
  src/io/file_storage.cpp
  src/io/server.cpp
  src/io/socket.cpp)
target_link_libraries(vsr_io PUBLIC vsr PRIVATE vsr_options)

option(VSR_BUILD_EXAMPLES "Build the examples" ON)
if(VSR_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

option(VSR_BUILD_FUZZ "Build the fuzz targets (Clang only)" OFF)
if(VSR_BUILD_FUZZ)
  add_subdirectory(fuzz)
endif()

# The simulator library `vsr_sim_lib` runs a host application in the
# simulator. A project that adds libvsr with `add_subdirectory` can link it
# without the tests.
option(VSR_BUILD_SIM "Build the simulator library and the simulator" ON)
if(VSR_BUILD_SIM OR VSR_BUILD_TESTS)
  add_subdirectory(sim)
endif()

if(VSR_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

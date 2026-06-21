cmake_minimum_required(VERSION 3.10...4.10)
project(mseedout VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)


# libmseed builds only static
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Disable shared libmseed" FORCE)
set(BUILD_STATIC_LIBS ON CACHE BOOL "Enable static libmseed" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "Disable libmseed examples" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "Disable libmseed tests" FORCE)

# libmseed
add_subdirectory(third_party/libmseed EXCLUDE_FROM_ALL)

include_directories(
    include
    third_party/libmseed
)

# Core library
add_library(mseedout_lib
    src/dp_packer.cpp
    src/processor.cpp
    src/mseedout.cpp
)
target_link_libraries(mseedout_lib mseed_static)
target_compile_options(mseedout_lib PRIVATE -O3 ${STRICT_WARNINGS})

# CLI executable
add_executable(mseedout src/main.cpp)
target_link_libraries(mseedout mseedout_lib mseed_static)
target_compile_options(mseedout PRIVATE -O3 ${STRICT_WARNINGS})

# Verification executable
add_executable(mseedverify src/verify.cpp)
target_link_libraries(mseedverify mseed_static)
target_compile_options(mseedverify PRIVATE ${STRICT_WARNINGS})

install(TARGETS mseedout DESTINATION bin)
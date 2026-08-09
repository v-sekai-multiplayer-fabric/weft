cmake_minimum_required(VERSION 3.16)
project(weft_dataplane CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(weft-dataplane src/main.cpp)

if(NOT WIN32)
  find_package(Threads REQUIRED)
  target_link_libraries(weft-dataplane PRIVATE Threads::Threads)
endif()

if(MSVC)
  target_compile_options(weft-dataplane PRIVATE /O2)
else()
  target_compile_options(weft-dataplane PRIVATE -O3 -funroll-loops)
endif()

include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_ok OUTPUT ipo_msg)
if(ipo_ok)
  set_property(TARGET weft-dataplane PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

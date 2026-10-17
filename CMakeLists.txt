cmake_minimum_required(VERSION 3.20)
project(absynth LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(MSVC)
  add_compile_options(/W4 /fp:fast /EHsc /permissive-)
else()
  add_compile_options(-Wall -Wextra -ffast-math -fno-exceptions -fno-rtti)
endif()

add_subdirectory(dsp)
add_subdirectory(dsp/tests)

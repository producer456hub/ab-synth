# The portable DSP core as a static library (header-only engine + the C ABI).
add_library(absynth STATIC src/c_api.cpp)
target_include_directories(absynth PUBLIC include)
target_compile_features(absynth PUBLIC cxx_std_17)

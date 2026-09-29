cmake_minimum_required(VERSION 3.22)
project(secretagent LANGUAGES C CXX VERSION 0.1.0)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Optional build surface. Embedding projects set this to OFF to skip every
# test executable.
option(SA_BUILD_TESTS "Build SecretAgent test binaries" ON)

# MSVC does not enable C11 <stdatomic.h> by default — the atomic_compat.h header
# in this project includes it unconditionally, so the /experimental:c11atomics
# flag is required on MSVC builds. Other compilers ship <stdatomic.h> by default.
# Apply it globally to every C target in this file before any add_library().
if(MSVC)
  add_compile_options(/experimental:c11atomics)
endif()

file(GLOB_RECURSE C_SRC "src/*/*.c")
file(GLOB_RECURSE C_INC "src/*/*.h")

include(CTest)
add_subdirectory(deps)

add_library(secretagent STATIC ${C_SRC})
target_include_directories(secretagent PRIVATE ${C_INC})

# Opt-in AddressSanitizer instrumentation of the secretagent library only (not
# external deps). /Z7 embeds debug info so ASan stack traces symbolicate.
# Default OFF; normal builds are unaffected. Test binary must also enable ASan
# so the runtime is pulled in at link time — see SA_ENABLE_ASAN in test/CMakeLists.txt.
option(SA_ENABLE_ASAN "Instrument the secretagent library with AddressSanitizer for debugging" OFF)
if(SA_ENABLE_ASAN)
  if(MSVC)
    target_compile_options(secretagent PRIVATE /fsanitize=address /Z7)
  else()
    target_compile_options(secretagent PRIVATE -fsanitize=address -fno-omit-frame-pointer -g)
    target_link_options(secretagent INTERFACE -fsanitize=address)
  endif()
endif()

# Optional UndefinedBehaviorSanitizer and ThreadSanitizer (GCC/Clang only).
# TSan and ASan are mutually exclusive — do not enable both in the same build.
option(SA_ENABLE_UBSAN "Instrument the secretagent library with UndefinedBehaviorSanitizer (GCC/Clang)" OFF)
if(SA_ENABLE_UBSAN AND NOT MSVC)
  target_compile_options(secretagent PRIVATE -fsanitize=undefined -fno-omit-frame-pointer -g)
  target_link_options(secretagent INTERFACE -fsanitize=undefined)
endif()

option(SA_ENABLE_TSAN "Instrument the secretagent library with ThreadSanitizer (GCC/Clang). Mutually exclusive with SA_ENABLE_ASAN." OFF)
if(SA_ENABLE_TSAN AND NOT MSVC)
  if(SA_ENABLE_ASAN)
    message(FATAL_ERROR "SA_ENABLE_TSAN and SA_ENABLE_ASAN are mutually exclusive; enable only one.")
  endif()
  target_compile_options(secretagent PRIVATE -fsanitize=thread -fno-omit-frame-pointer -g)
  target_link_options(secretagent INTERFACE -fsanitize=thread)
endif()

if(SA_BUILD_TESTS)
  add_subdirectory(test)
endif()
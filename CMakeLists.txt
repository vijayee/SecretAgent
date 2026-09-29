cmake_minimum_required(VERSION 3.22)
project(secretagent LANGUAGES C CXX VERSION 0.1.0)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Export compile_commands.json by default so clangd/IDE tooling works out of
# the box (no-op with the Visual Studio generator).
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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

# Optional Python execution layer: CPython 3.12 built from the pinned
# deps/cpython submodule via ExternalProject (the liboffs external-dep
# pattern). OFF keeps the library libpython-free for embedders that do
# not host Python.
option(SA_ENABLE_PYTHON "Build the Python execution layer (requires deps/cpython)" ON)
if(SA_ENABLE_PYTHON)
  if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/deps/cpython/Include/Python.h)
    message(FATAL_ERROR "deps/cpython submodule missing. Run: git submodule update --init --recursive")
  endif()
  include(ExternalProject)
  set(CPYTHON_ROOT    ${CMAKE_CURRENT_SOURCE_DIR}/deps/cpython)
  set(CPYTHON_BUILD   ${CMAKE_BINARY_DIR}/deps/cpython)
  set(CPYTHON_CONFIGURE ${CMAKE_COMMAND} -E chdir ${CPYTHON_BUILD} bash ${CPYTHON_ROOT}/configure --without-ensurepip --disable-test-modules)
  set(CPYTHON_INSTALL ${CMAKE_COMMAND} -E echo "Skipping install; consuming build-dir artifacts directly")
  ExternalProject_Add(cpython-ext
    SOURCE_DIR ${CPYTHON_ROOT}
    CONFIGURE_COMMAND ${CPYTHON_CONFIGURE}
    BUILD_COMMAND make -j
    INSTALL_COMMAND ${CPYTHON_INSTALL}
    BUILD_BYPRODUCTS ${CPYTHON_BUILD}/libpython3.12.a
    BINARY_DIR ${CPYTHON_BUILD}
  )
  # NOTE: bumping the deps/cpython pin does not touch these ExternalProject
  # stamps — after a re-pin, remove the old build stamps or the whole
  # ${CMAKE_BINARY_DIR}/deps/cpython tree to force a fresh configure+build.
endif()
if(NOT SA_ENABLE_PYTHON)
  list(FILTER C_SRC EXCLUDE REGEX "src/Python/")
endif()

add_library(secretagent STATIC ${C_SRC})
target_include_directories(secretagent PRIVATE ${C_INC})

if(SA_ENABLE_PYTHON)
  # CMake-level mirror of the PUBLIC compile definition below; the test
  # subdirectory adds pyrt test sources with a plain if(SA_HAS_PYTHON) gate.
  set(SA_HAS_PYTHON ON)
  target_compile_definitions(secretagent PUBLIC SA_HAS_PYTHON=1)
  target_include_directories(secretagent PRIVATE ${CPYTHON_ROOT}/Include ${CPYTHON_BUILD})
  # Root cause of embedded stdlib discovery: a statically linked libpython has
  # no executable of its own, so CPython's getpath has no anchor for its
  # build-directory landmark (pybuilddir.txt) and would fall back to whatever
  # a PATH search happens to find on the host. Pointing config.executable at
  # the build-tree `python` pins the paths to this build's source Lib dir and
  # compiled-modules dir deterministically. The path is absolute per build, so
  # PRIVATE scope is sufficient.
  target_compile_definitions(secretagent PRIVATE
    "SA_PYTHON_EXECUTABLE=\"${CPYTHON_BUILD}/python\"")
  # PUBLIC: any host embedding the static library carries the CPython runtime
  # (pyrt is part of the library ABI while SA_HAS_PYTHON is defined).
  target_link_libraries(secretagent PUBLIC ${CPYTHON_BUILD}/libpython3.12.a)
  target_link_libraries(secretagent PUBLIC crypt dl m)
  target_link_libraries(secretagent PUBLIC pthread)
  add_dependencies(secretagent cpython-ext)
endif()

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
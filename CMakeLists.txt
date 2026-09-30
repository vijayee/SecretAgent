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

# Declared before add_subdirectory(deps) so deps/CMakeLists.txt can gate the
# in-tree http_parser static library on the same switch the root uses for the
# poll-dancer ExternalProject (see the streams block below the WaveDB one).
option(SA_ENABLE_STREAMS "Build the streams/http transport (requires deps/poll-dancer, deps/http-parser)" ON)

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

 # Streams/http transport family port (src/Streams + its src/Util, src/Buffer,
 # src/Platform closure). The C_SRC glob above picks the ported sources up
 # globally; when the gate is OFF they are excluded the same way the Python
 # layer is, so an OFF build stays sans streams.
if(NOT SA_ENABLE_STREAMS)
  list(FILTER C_SRC EXCLUDE REGEX "src/Streams/|src/Buffer/")
  list(FILTER C_SRC EXCLUDE REGEX "src/Util/(vec|validation|error|bcrypt)\\.c$")
  list(FILTER C_SRC EXCLUDE
    REGEX "src/Platform/(platform_random|platform_file|platform_socket)\\.c$")
endif()

# WaveDB: the durable floor (embedded HBTrie/MVCC/WAL/subtree store). Built
# from the pinned submodule via ExternalProject (same pattern as cpython-ext).
# OFF keeps the library wavedb-free for embedders that only need the actor
# runtime.
option(SA_ENABLE_WDB "Build the WaveDB-backed frame store (requires deps/wavedb)" ON)
if(SA_ENABLE_WDB)
  if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/deps/wavedb/CMakeLists.txt)
    message(FATAL_ERROR "deps/wavedb submodule missing. Run: git submodule update --init --recursive")
  endif()
  include(ExternalProject)
  set(WDB_ROOT    ${CMAKE_CURRENT_SOURCE_DIR}/deps/wavedb)
  set(WDB_BUILD   ${CMAKE_BINARY_DIR}/deps/wavedb)
  # WaveDB's REAL option names (grep CMakeLists.txt): BUILD_TESTS (default ON)
  # plus BUILD_EXAMPLES/BUILD_STRESS_TESTS/BUILD_BENCHMARKS and the
  # BUILD_NODEJS_/BUILD_DART_/BUILD_PYTHON_BINDINGS shared-FFI toggles. We are
  # C-side only, so every one of them stays off.
  set(WDB_CONFIGURE ${CMAKE_COMMAND} -S ${WDB_ROOT} -B ${WDB_BUILD}
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON
      -DBUILD_TESTS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_STRESS_TESTS=OFF
      -DBUILD_BENCHMARKS=OFF -DBUILD_NODEJS_BINDINGS=OFF
      -DBUILD_DART_BINDINGS=OFF -DBUILD_PYTHON_BINDINGS=OFF)
  set(WDB_INSTALL ${CMAKE_COMMAND} -E echo "Skipping install; consuming build-dir artifacts directly")
  ExternalProject_Add(wavedb-ext
    SOURCE_DIR ${WDB_ROOT}
    CONFIGURE_COMMAND ${WDB_CONFIGURE}
    BUILD_COMMAND ${CMAKE_COMMAND} --build ${WDB_BUILD} --target wavedb
    INSTALL_COMMAND ${WDB_INSTALL}
    BUILD_BYPRODUCTS ${WDB_BUILD}/libwavedb.a
    BINARY_DIR ${WDB_BUILD}
  )
  # NOTE: bumping the deps/wavedb pin does not touch these ExternalProject
  # stamps — if configure does not re-run after a re-pin, wipe
  # ${CMAKE_BINARY_DIR}/deps/wavedb to force a fresh configure+build.

  # WaveDB descends from the same extracted-runtime family as this repo's
  # src/Platform, src/Util, and src/RefCounter, and its archive bundles its own
  # copies of those shared helpers (get_memory/get_clear_memory, log_*,
  # refcounter_*, platform_barrier_*, platform_core_count). Linking both
  # archives into one binary — every test binary, any embedder — fails with
  # multiple-definition errors, and letting one side silently win would route
  # cross-type calls over mismatched struct layouts (WaveDB's barriers are raw
  # pthread_barrier_t; ours are opaque platform_barrier_t). The link-correct
  # fix, without touching the pinned submodule: post-process a build-dir COPY
  # of libwavedb.a with objcopy --redefine-syms, aliasing WaveDB's family
  # copies under wavedb_-prefixed names. objcopy rewrites definitions AND
  # internal references inside the archive, so WaveDB keeps running its own
  # implementations unchanged and SecretAgent keeps running its own; the two
  # symbol sets become provably disjoint (the list below is the exact nm(1)
  # intersection of the two archives — grown when the streams port added the
  # family vec/buffer members under src/Util and src/Buffer, whose liboffs
  # revision carries a different buffer_t layout: a shared unaliased
  # buffer_create would silently route WaveDB calls across mismatched
  # struct layouts).
  # Probe for a usable objcopy BEFORE committing to the alias path: MSVC
  # toolchains may ship no objcopy at all, and linking the raw archive
  # would collide on the family-shared symbols — so such a build fails
  # loudly HERE with a real message instead of a missing-tool error some
  # custom-command stage in. Toolchains that always carry objcopy (Linux,
  # macOS) take the REQUIRED form.
  if(WIN32)
    find_program(SA_OBJCOPY NAMES objcopy llvm-objcopy)
    if(NOT SA_OBJCOPY)
      message(FATAL_ERROR
        "SA_ENABLE_WDB needs objcopy (or llvm-objcopy) to alias WaveDB's "
        "family-shared symbols; unavailable on this Windows toolchain")
    endif()
  else()
    find_program(SA_OBJCOPY NAMES objcopy llvm-objcopy REQUIRED)
  endif()
  set(_wdb_alias_dir ${CMAKE_BINARY_DIR}/deps/wavedb-alias)
  set(_wdb_alias_syms ${_wdb_alias_dir}/family-shared.syms)
  set(_wdb_alias_archive ${_wdb_alias_dir}/libwavedb-alias.a)
  file(WRITE ${_wdb_alias_syms}
"get_memory wavedb_get_memory
get_clear_memory wavedb_get_clear_memory
log_add_callback wavedb_log_add_callback
log_add_fp wavedb_log_add_fp
log_level_string wavedb_log_level_string
log_log wavedb_log_log
log_set_level wavedb_log_set_level
log_set_lock wavedb_log_set_lock
log_set_quiet wavedb_log_set_quiet
platform_barrier_destroy wavedb_platform_barrier_destroy
platform_barrier_wait wavedb_platform_barrier_wait
platform_core_count wavedb_platform_core_count
refcounter_consume wavedb_refcounter_consume
refcounter_count wavedb_refcounter_count
refcounter_dereference wavedb_refcounter_dereference
refcounter_init wavedb_refcounter_init
refcounter_reference wavedb_refcounter_reference
refcounter_yield wavedb_refcounter_yield
vec_compact_ wavedb_vec_compact_
vec_expand_ wavedb_vec_expand_
vec_insert_ wavedb_vec_insert_
vec_reserve_ wavedb_vec_reserve_
vec_reserve_po2_ wavedb_vec_reserve_po2_
vec_splice_ wavedb_vec_splice_
vec_swap_ wavedb_vec_swap_
vec_swapsplice_ wavedb_vec_swapsplice_
buffer_and wavedb_buffer_and
buffer_compare wavedb_buffer_compare
buffer_concat wavedb_buffer_concat
buffer_copy wavedb_buffer_copy
buffer_copy_from_pointer wavedb_buffer_copy_from_pointer
buffer_create wavedb_buffer_create
buffer_create_from_existing_memory wavedb_buffer_create_from_existing_memory
buffer_create_from_pointer_copy wavedb_buffer_create_from_pointer_copy
buffer_destroy wavedb_buffer_destroy
buffer_get_index wavedb_buffer_get_index
buffer_not wavedb_buffer_not
buffer_or wavedb_buffer_or
buffer_set_index wavedb_buffer_set_index
buffer_slice wavedb_buffer_slice
buffer_xor wavedb_buffer_xor
")
  add_custom_command(OUTPUT ${_wdb_alias_archive}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${_wdb_alias_dir}
    COMMAND ${SA_OBJCOPY} --redefine-syms=${_wdb_alias_syms}
            ${WDB_BUILD}/libwavedb.a ${_wdb_alias_archive}
    DEPENDS ${_wdb_alias_syms} ${WDB_BUILD}/libwavedb.a
    VERBATIM
    COMMENT "Aliasing WaveDB's family-shared symbols (wavedb_ prefix)")
  add_custom_target(wavedb-alias DEPENDS ${_wdb_alias_archive})
  add_dependencies(wavedb-alias wavedb-ext)
  set(WDB_LINK_ARCHIVE ${_wdb_alias_archive})
  set(WDB_ALIAS_TARGET wavedb-alias)
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

if(SA_ENABLE_WDB)
  # CMake-level mirror of the PUBLIC compile definition below; the test
  # subdirectory gates frame sources with a plain if(SA_HAS_WDB) check.
  set(SA_HAS_WDB ON)
  target_compile_definitions(secretagent PUBLIC SA_HAS_WDB=1)
  target_include_directories(secretagent PRIVATE ${WDB_ROOT}/src)
  target_link_libraries(secretagent PUBLIC ${WDB_LINK_ARCHIVE})
  add_dependencies(secretagent wavedb-ext ${WDB_ALIAS_TARGET})
  # WaveDB's public headers reach into its third-party deps, and its archive
  # leaves some of those deps as undefined symbols (Task 1 review findings,
  # verified on this host): <cbor.h> from libcbor, the generated + vendored
  # libcbor trees, the hashmap include dir, and the xxhash/libcbor/libhashmap
  # archives (plus pthread/m/atomic) must be present when the frame store
  # pulls real WaveDB objects into the link. Order matters for archives:
  # libwavedb.a first, then the archives it depends on.
  target_include_directories(secretagent PRIVATE
    ${WDB_ROOT}/deps/libcbor/src
    ${WDB_BUILD}/deps/libcbor
    ${WDB_BUILD}/deps/libcbor/src
    ${WDB_ROOT}/deps/hashmap/include)
  target_link_libraries(secretagent PUBLIC
    ${WDB_BUILD}/libxxhash.a
    ${WDB_BUILD}/deps/libcbor/src/libcbor.a
    ${WDB_BUILD}/libhashmap.a
    pthread m atomic)
  # WaveDB's encryption.c calls the OpenSSL EVP_* API; with encryption code
  # compiled in, the archive carries undefined EVP symbols and linkers reject
  # the binary unless libcrypto follows. WaveDB's own CMake does the same
  # find_package dance, so mirror it and link only when the host has Crypto.
  find_package(OpenSSL COMPONENTS Crypto)
  if(OpenSSL_FOUND)
    target_link_libraries(secretagent PUBLIC OpenSSL::Crypto)
  endif()
endif()

# poll-dancer: the IO event loop (epoll/kqueue/iocp) the streams/http
# transport runs on. Built from the pinned submodule via ExternalProject
# (same pattern as wavedb-ext). Backend selection is poll-dancer's own CMake
# (platform-based); its real option names are POLL_DANCER_BUILD_TESTS /
# BUILD_EXAMPLES / BUILD_SHARED / THREAD_SAFE, and its static target is
# `poll_dancer` (OUTPUT_NAME already poll_dancer, so the artifact is
# libpoll_dancer.a). OFF keeps the library streams-free for embedders that
# only need the actor runtime.
if(SA_ENABLE_STREAMS)
  if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/deps/poll-dancer/CMakeLists.txt)
    message(FATAL_ERROR "deps/poll-dancer submodule missing. Run: git submodule update --init --recursive")
  endif()
  include(ExternalProject)
  set(PD_ROOT  ${CMAKE_CURRENT_SOURCE_DIR}/deps/poll-dancer)
  set(PD_BUILD ${CMAKE_BINARY_DIR}/deps/poll-dancer)
  set(PD_CONFIGURE ${CMAKE_COMMAND} -S ${PD_ROOT} -B ${PD_BUILD}
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON
      -DPOLL_DANCER_BUILD_TESTS=OFF -DPOLL_DANCER_BUILD_EXAMPLES=OFF
      -DPOLL_DANCER_BUILD_SHARED=OFF -DPOLL_DANCER_THREAD_SAFE=ON)
  set(PD_INSTALL ${CMAKE_COMMAND} -E echo "Skipping install; consuming build-dir artifacts directly")
  ExternalProject_Add(poll-dancer-ext
    SOURCE_DIR ${PD_ROOT}
    CONFIGURE_COMMAND ${PD_CONFIGURE}
    BUILD_COMMAND ${CMAKE_COMMAND} --build ${PD_BUILD} --target poll_dancer
    INSTALL_COMMAND ${PD_INSTALL}
    BUILD_BYPRODUCTS ${PD_BUILD}/libpoll_dancer.a
    BINARY_DIR ${PD_BUILD}
  )
  # NOTE: bumping the deps/poll-dancer pin does not touch these ExternalProject
  # stamps — after a re-pin, wipe ${CMAKE_BINARY_DIR}/deps/poll-dancer to
  # force a fresh configure+build.

  # One gate for the whole streams surface: the library-side additions below
  # consume the PD_ROOT/PD_BUILD set above, and a single if() block makes the
  # pairing immune to reordering.
  # CMake-level mirror of the PUBLIC compile definition below; test sources
  # gate streams additions with a plain if(SA_HAS_STREAMS) check.
  set(SA_HAS_STREAMS ON)
  target_compile_definitions(secretagent PUBLIC SA_HAS_STREAMS=1)
  # PUBLIC: SA_HAS_STREAMS=1 propagates PUBLIC too, so consumers (test TUs)
  # that include src/Platform/platform.h reach its gated platform_socket.h
  # include, which in turn reaches <poll-dancer/poll-dancer.h>.
  target_include_directories(secretagent PUBLIC ${PD_ROOT}/include)
  # The dep archives link now (an unused static archive pulls nothing, so the
  # link stays clean until src/Streams consumes them); the transport sources
  # ride this same gate from the port task on. http_parser is defined in
  # deps/CMakeLists.txt (plain static lib — upstream has no CMakeLists, gyp
  # only); linking it transmits its PUBLIC include dir, so no extra include
  # path is added here.
  target_link_libraries(secretagent PUBLIC ${PD_BUILD}/libpoll_dancer.a http_parser)
  # poll_dancer is configured with THREAD_SAFE=ON, so its objects reference
  # pthread; the link needs -lpthread exactly like the wavedb and python
  # blocks above.
  target_link_libraries(secretagent PUBLIC pthread)
  add_dependencies(secretagent poll-dancer-ext)

  # The ported streams/Buffer family files live in their own dirs; the
  # C_SRC/C_INC glob already carries src/Buffer/buffer.h, and the HTTP core
  # reaches it as "../Buffer/buffer.h", but make the Buffer include dir
  # explicit here so the ported sources also compile against it directly.
  target_include_directories(secretagent PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/Buffer)

  # The ported http_connection/http_server include <openssl/ssl.h>
  # unconditionally (TLS connections are part of the copied core), so the
  # streams gate carries a hard OpenSSL requirement — liboffs does the same
  # (find_package(OpenSSL REQUIRED)). OpenSSL::Crypto is linked alongside
  # SSL to match liboffs's link set.
  find_package(OpenSSL REQUIRED COMPONENTS SSL Crypto)
  target_link_libraries(secretagent PUBLIC OpenSSL::SSL OpenSSL::Crypto)

  # liboffs's vendored Solar Designer crypt_blowfish backs src/Util/bcrypt.c
  # (bcrypt_check/bcrypt_generate call _crypt_blowfish_rn and
  # _crypt_gensalt_blowfish_rn); the ported copy compiled from
  # src/Streams/crypt_blowfish.c rides this same gate (glob-included).
  # The ported Buffer/buffer.c also calls libcbor (buffer_to_cbor/
  # cbor_to_buffer); libcbor's headers and archive reach it through the
  # wavedb block's include dirs and target libraries above, so the streams
  # gate requires SA_ENABLE_WDB. Fail loudly instead of a mid-link surprise.
  if(NOT SA_ENABLE_WDB)
    message(FATAL_ERROR
      "SA_ENABLE_STREAMS requires SA_ENABLE_WDB: the ported Buffer closure "
      "(buffer_to_cbor/cbor_to_buffer) consumes libcbor built by wavedb")
  endif()
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

# frame-demo (Task 11): the demo CLI. One executable, no tests: it needs the
# frame store (WaveDB gate — frame.h's whole surface rides SA_HAS_WDB) and
# the model client; the python bridge arrives through the same library link.
# SA_HAS_WDB/SA_HAS_PYTHON propagate from the PUBLIC compile definitions of
# secretagent; the executable's main.c reaches py_agent_init through the
# bare-extern idiom so it needs no CPython include dirs.
if(SA_ENABLE_WDB)
  add_executable(frame-demo tools/frame-demo/main.c)
  target_link_libraries(frame-demo PRIVATE secretagent)
  if(NOT WIN32)
    target_link_libraries(frame-demo PRIVATE pthread)
  endif()
  target_include_directories(frame-demo PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

  # Match the sanitizer gates from test/CMakeLists.txt: the library's
  # INTERFACE link option pulls the runtime in, but main.c itself stays
  # uninstrumented without matching compile options — mirror them so bugs in
  # the demo surface under the sanitizer builds too.
  if(SA_ENABLE_ASAN)
    if(MSVC)
      target_compile_options(frame-demo PRIVATE /fsanitize=address /Z7)
      target_link_options(frame-demo PRIVATE /DEBUG)
    else()
      target_compile_options(frame-demo PRIVATE -fsanitize=address -fno-omit-frame-pointer -g)
      target_link_options(frame-demo PRIVATE -fsanitize=address)
    endif()
  endif()
  if(SA_ENABLE_UBSAN AND NOT MSVC)
    target_compile_options(frame-demo PRIVATE -fsanitize=undefined -fno-omit-frame-pointer -g)
    target_link_options(frame-demo PRIVATE -fsanitize=undefined)
  endif()
  if(SA_ENABLE_TSAN AND NOT MSVC)
    if(SA_ENABLE_ASAN)
      message(FATAL_ERROR "SA_ENABLE_TSAN and SA_ENABLE_ASAN are mutually exclusive; enable only one.")
    endif()
    target_compile_options(frame-demo PRIVATE -fsanitize=thread -fno-omit-frame-pointer -g)
    target_link_options(frame-demo PRIVATE -fsanitize=thread)
  endif()
endif()

if(SA_BUILD_TESTS)
  add_subdirectory(test)
endif()
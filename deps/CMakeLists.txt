set(CMAKE_CXX_STANDARD 17)

# GoogleTest is a source-level submodule dep (deps/googletest, pinned in
# .gitmodules) — the same pattern liboffs uses. It is consumed from
# test/CMakeLists.txt against GTest::gtest_main / GTest::gmock targets.
add_subdirectory(googletest)

# Streams-transport deps (SA_ENABLE_STREAMS is declared in the root
# CMakeLists.txt before this add_subdirectory so both files gate on it).

# http-parser: upstream carries NO CMakeLists (gyp only), so it is compiled
# here as a plain static library from the pinned submodule rather than via
# ExternalProject.
if(SA_ENABLE_STREAMS)
  if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/http-parser/http_parser.c)
    message(FATAL_ERROR "deps/http-parser submodule missing. Run: git submodule update --init --recursive")
  endif()
  add_library(http_parser STATIC ${CMAKE_CURRENT_SOURCE_DIR}/http-parser/http_parser.c)
  set_property(TARGET http_parser PROPERTY POSITION_INDEPENDENT_CODE ON)
  target_include_directories(http_parser PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/http-parser)

  # pcre2 (deps/pcre2 submodule, pinned): WIN32-only build path. Task 2 ports
  # platform_regex_compat.h, whose _WIN32 path maps POSIX regex onto PCRE2's
  # pcre2posix; Linux uses <regex.h>, so this host never needs pcre2 and the
  # submodule is pinned without a build step — pcre2's build is wired only
  # then, when a Windows toolchain exists, in poll-dancer/liboffs's own shape:
  # an ExternalProject
  # producing the pcre2-8 static archive plus pcre2-posix with
  # PCRE2_SUPPORT_JIT, consumed only inside if(WIN32). Kept as documented
  # shape only — no guarded code runs anywhere today:
  #
  #   if(WIN32)
  #     ... pcre2-8 + pcre2-posix static build, PCRE2_SUPPORT_JIT=ON ...
  #   endif()
endif()

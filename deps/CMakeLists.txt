set(CMAKE_CXX_STANDARD 17)

# GoogleTest is a source-level submodule dep (deps/googletest, pinned in
# .gitmodules) — the same pattern liboffs uses. It is consumed from
# test/CMakeLists.txt against GTest::gtest_main / GTest::gmock targets.
add_subdirectory(googletest)
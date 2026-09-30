if (BUILD_TESTING)
    # BUILD_TESTING is the liboffs-inherited defensive inner gate; the parent's
    # SA_BUILD_TESTS option is the authoritative switch for building tests.
    project(testsecretagent C CXX)
    set(CMAKE_C_STANDARD 11)
    set(CMAKE_CXX_STANDARD 17)

    # Build googletest if not already found
    if (NOT TARGET GTest::gtest_main)
      set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
      add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../deps/googletest ${CMAKE_BINARY_DIR}/deps/googletest)
    endif()

    # The /experimental:c11atomics flag is set globally in the top-level
    # CMakeLists.txt and propagates here through add_compile_options.

    # MSVC AddressSanitizer sets _ANNOTATE_STRING/_ANNOTATE_VECTOR=1 on every
    # object compiled with /fsanitize=address, and the linker enforces a single
    # value across all C++/STL objects (LNK2038 otherwise). gtest/gmock are C++
    # with heavy STL use and are built in-tree here, so under SA_ENABLE_ASAN
    # they must be instrumented too to match the ASan testsecretagent objects.
    if(SA_ENABLE_ASAN)
      foreach(_gtest_t gtest gtest_main gmock gmock_main)
        if(TARGET ${_gtest_t})
          if(MSVC)
            target_compile_options(${_gtest_t} PRIVATE /fsanitize=address /Z7)
          else()
            target_compile_options(${_gtest_t} PRIVATE -fsanitize=address -fno-omit-frame-pointer -g)
          endif()
        endif()
      endforeach()
    endif()

    add_executable(testsecretagent test_main.cpp
            test_actor.cpp
            test_message_queue.cpp
            test_pool.cpp
            test_deque.cpp
            test_scheduler.cpp
            test_refcounter.cpp
            test_json.cpp
            test_http.cpp
            )

    # Same CMake-level mirror idiom as the pyrt gate above: frame store tests
    # only exist while the WaveDB-backed store is compiled in.
    if(SA_HAS_WDB)
      target_sources(testsecretagent PRIVATE test_frame.cpp)
    endif()

    if(SA_HAS_PYTHON)
      target_sources(testsecretagent PRIVATE test_pyrt.cpp)

      # The bridge verbs run on the injected module and answer through the
      # frame_bridge registry, so this suite needs CPython but not WaveDB
      # (the scripted owner answers by hand — no store involved).
      # test_py_agent.cpp includes py_agent.h, which includes Python.h; the
      # CPython include dirs are PRIVATE on the secretagent target, so this
      # test target mirrors them (libpython itself arrives via the PUBLIC
      # link of the parent target).
      if(SA_ENABLE_PYTHON)
        target_include_directories(testsecretagent PRIVATE
          ${CPYTHON_ROOT}/Include ${CPYTHON_BUILD})
      endif()
      target_sources(testsecretagent PRIVATE test_py_agent.cpp)

      # Order-isolated pyrt pool-cap unit: the global interpreter pool cap is
      # fixed by the FIRST pyrt created in the process, so the cap=1 test must
      # run in a binary where it is the only runtime creator. Same link shape
      # as testsecretagent; gtest_main supplies the runner (no test_main.cpp).
      add_executable(testpyrtpoolcap test_pyrt_poolcap.cpp)
      target_link_libraries(testpyrtpoolcap PRIVATE secretagent)
      if(NOT WIN32)
        target_link_libraries(testpyrtpoolcap PRIVATE pthread)
      endif()
      target_link_libraries(testpyrtpoolcap PRIVATE GTest::gtest_main)
      target_include_directories(testpyrtpoolcap PRIVATE ${C_INC})
      target_include_directories(testpyrtpoolcap PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)

      include(GoogleTest)
      gtest_add_tests(TARGET testpyrtpoolcap TEST_LIST test_list_poolcap)
      set_tests_properties(${test_list_poolcap} PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
    endif()

    # Match the SA_ENABLE_ASAN gate from the parent CMakeLists.txt: when on,
    # instrument the test exe and pull in the ASan runtime.
    if(SA_ENABLE_ASAN)
      if(MSVC)
        target_compile_options(testsecretagent PRIVATE /fsanitize=address /Z7)
        target_link_options(testsecretagent PRIVATE /DEBUG)
      else()
        target_compile_options(testsecretagent PRIVATE -fsanitize=address -fno-omit-frame-pointer -g)
        target_link_options(testsecretagent PRIVATE -fsanitize=address)
      endif()
    endif()
    if(SA_ENABLE_UBSAN AND NOT MSVC)
      target_compile_options(testsecretagent PRIVATE -fsanitize=undefined -fno-omit-frame-pointer -g)
      target_link_options(testsecretagent PRIVATE -fsanitize=undefined)
    endif()
    if(SA_ENABLE_TSAN AND NOT MSVC)
      if(SA_ENABLE_ASAN)
        message(FATAL_ERROR "SA_ENABLE_TSAN and SA_ENABLE_ASAN are mutually exclusive; enable only one.")
      endif()
      target_compile_options(testsecretagent PRIVATE -fsanitize=thread -fno-omit-frame-pointer -g)
      target_link_options(testsecretagent PRIVATE -fsanitize=thread)
    endif()

    target_link_libraries(testsecretagent PRIVATE secretagent)
    # Scheduler/pool use pthreads directly; liboffs wraps this in NOT WIN32.
    if(NOT WIN32)
      target_link_libraries(testsecretagent PRIVATE pthread)
    endif()
    target_link_libraries(testsecretagent PRIVATE GTest::gtest_main)
    target_link_libraries(testsecretagent PRIVATE GTest::gmock)

    # C_INC is a header-file glob (inert as an include dir; copied verbatim from
    # liboffs). Real cross-module includes are the quoted ../src/<Module> paths.
    target_include_directories(testsecretagent PRIVATE ${C_INC})
    target_include_directories(testsecretagent PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)

    include(GoogleTest)
    gtest_add_tests(TARGET testsecretagent TEST_LIST test_list)
    set_tests_properties(${test_list} PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

    # Under SA_ENABLE_ASAN the pyrt tests legitimately exit with CPython-owned
    # allocations still on the table (see lsan-cpython.supp for the documented
    # classes: pymalloc arena bookkeeping, object pools, interpreter-internal
    # list storage, thread locks). LSan reports those at exit and fails the
    # suite, so the pyrt tests carry the suppression file; the patterns only
    # match obmalloc/listobject/thread_pthread frames, so a pyrt-owned leak is
    # still reported and still fails the run. Non-pyrt tests keep full leak
    # checking.
    if(SA_ENABLE_ASAN AND SA_HAS_PYTHON)
      set(_sa_lsan_supp "${CMAKE_CURRENT_SOURCE_DIR}/lsan-cpython.supp")
      foreach(_test_name ${test_list} ${test_list_poolcap})
        if(_test_name MATCHES "^TestPyrt")
          set_property(TEST ${_test_name} APPEND PROPERTY ENVIRONMENT "LSAN_OPTIONS=suppressions=${_sa_lsan_supp}")
        endif()
      endforeach()
      unset(_test_name)
    endif()
endif()
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
            )

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
endif()
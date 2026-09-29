if (BUILD_TESTING)
    project(testsecretagent C CXX)
    set(CMAKE_C_STANDARD 11)
    set(CMAKE_CXX_STANDARD 17)

    # Build googletest if not already found
    if (NOT TARGET GTest::gtest_main)
      set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
      add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../deps/googletest ${CMAKE_BINARY_DIR}/deps/googletest)
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
    target_link_libraries(testsecretagent PRIVATE pthread)
    target_link_libraries(testsecretagent PRIVATE GTest::gtest_main)
    target_link_libraries(testsecretagent PRIVATE GTest::gmock)

    target_include_directories(testsecretagent PUBLIC ${C_INC})
    target_include_directories(testsecretagent PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)

    include(GoogleTest)
    gtest_add_tests(TARGET testsecretagent TEST_LIST test_list)
    set_tests_properties(${test_list} PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
endif()
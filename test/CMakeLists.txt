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
            # The lifecycle suite is PURE (cursor fold + closers over a json
            # array text): no WDB, no streams — it registers on the plain
            # gate so the OFF build keeps it.
            test_lifecycle.cpp
            # The budget suite is PURE (Util constants + the marker helper):
            # no WDB, no streams — same plain gate as test_lifecycle.
            test_budget.cpp
            # The guards suite is PURE (the doom fold + the retry table):
            # no WDB, no streams.
            test_guards.cpp
            # The persona suite is PURE (the record load + the compose over
            # Util/json): no WDB, no streams — same plain gate.
            test_persona.cpp
            # The stream-framer suite is PURE (the ported byte-framer over
            # Util/allocator): no WDB, no streams — same plain gate.
            test_stream_framer.cpp
            )

    # Same CMake-level mirror idiom as the pyrt gate above: frame store tests
    # only exist while the WaveDB-backed store is compiled in. The model
    # client rides on frame_config_t (a WaveDB-era member of frame.h), so its
    # tests share that gate.
    if(SA_HAS_WDB)
      target_sources(testsecretagent PRIVATE test_frame.cpp)
      # The WaveDB reopen-amnesia probe (the recorded substrate defect):
      # WaveDB's API opened DIRECTLY, on the aliased archive this harness
      # links — the thinnest reproducer for bisecting the defect.
      target_sources(testsecretagent PRIVATE test_wdb_reopen_probe.cpp)
      # The reopen probe opens WaveDB's API directly (thinnest reproducer):
      # its header chain needs src's module dirs PLUS the WDB-facing
      # third-party dirs the root's SA_ENABLE_WDB block gives the library
      # (Database/database.h -> HBTrie/identifier.h -> <cbor.h>). Its OWN
      # gate is this WDB gate — a WDB-only test must not sit behind the
      # streams gate even though the same probe's older copy once rode it
      # (a streams-off/WDB-on configure registers the probe with no include
      # path otherwise: test_wdb_reopen_probe.cpp fails at Database/database.h,
      # then at <cbor.h>).
      target_include_directories(testsecretagent PRIVATE
        ${WDB_ROOT}/src
        ${WDB_ROOT}/deps/libcbor/src
        ${WDB_BUILD}/deps/libcbor
        ${WDB_BUILD}/deps/libcbor/src
        ${WDB_ROOT}/deps/hashmap/include)
      # The client-api wire suite (Task 2): PURE over libcbor — no store, no
      # streams — but it does NOT ride the plain gate: the wire's only dep is
      # libcbor, and in this ecosystem libcbor arrives through the wavedb gate
      # (wavedb-ext builds deps/wavedb's vendored copy; the OFF build with
      # SA_ENABLE_WDB=OFF has neither the include dirs nor the archive). So
      # the suite registers here on the WDB gate and shares the cbor include
      # dirs the reopen probe above pulls in.
      target_sources(testsecretagent PRIVATE test_client_api_wire.cpp)
      # The client-api handlers' suite (Task 4): the in-proc wiring needs the
      # frames (WDB) AND the loop thread (streams) — its gate is BOTH (the
      # OFF-with-streams build has no libcbor either, so it rides the WDB
      # check and, inside it, the streams one).
      if(SA_HAS_STREAMS)
        target_sources(testsecretagent PRIVATE test_client_api_handlers.cpp)
      endif()
      # The client-api TRANSPORTS' suite (Tasks 5 + 6, one file for both
      # transports): the socket carriers need everything the handlers' suite
      # does (the wire's libcbor + the loop thread's poll-dancer) plus the
      # framer and the bcrypt machinery — the same BOTH gate.
      if(SA_HAS_STREAMS)
        target_sources(testsecretagent PRIVATE
                       test_client_api_transports.cpp)
        # The typed C client's suite (Task 7): the REAL transports + handlers
        # server in-proc driven by sa_client over the REAL socket — the same
        # BOTH gate as the transports' suite (the module self-gates its
        # library TU identically, so the OFF build needs no regex).
        target_sources(testsecretagent PRIVATE test_sa_client.cpp)
      endif()
      # The refine slice rides the same WDB gate as test_frame (its types live
      # on frame.h's surface); the review-call tests also need the streams
      # gate, added with Task 4's step.
      if(SA_HAS_STREAMS)
        target_sources(testsecretagent PRIVATE test_refine.cpp)
      endif()
      # The model client's real-http tests drive model_http_backend_create —
      # which rides the streams transport in model.c — so they need the
      # streams gate too, not just the WDB gate their fixtures share.
      if(SA_HAS_STREAMS)
        target_sources(testsecretagent PRIVATE test_model_decode.cpp)
      endif()
      # The loop suite rides both gates internally: restart/replay and the
      # content-only paths are WDB-only; the cell-running tests nest the
      # real `#if defined(SA_HAS_WDB) && defined(SA_HAS_PYTHON)` form inside.
      target_sources(testsecretagent PRIVATE test_loop.cpp)
      # The live integration gate (Task 12): registered unconditionally, but
      # the test body GTEST_SKIPs unless SA_TEST_OLLAMA_URL names an
      # OpenAI-compatible endpoint — no network in a default run. It shares
      # the same store gate as test_loop.cpp, and its real test rides the
      # python gate in-file (it drives actual cells).
      target_sources(testsecretagent PRIVATE test_live_loop.cpp)
    endif()

    # The ported streams server suite rides the streams gate (same CMake-level
    # SA_HAS_STREAMS variable the root sets; the ported server core needs
    # poll-dancer + OpenSSL + libcbor, and libcbor arrives via SA_ENABLE_WDB).
    if(SA_HAS_STREAMS)
      target_sources(testsecretagent PRIVATE test_streams_server.cpp)
      # The async client suite: real loop thread + real loopback fake servers
      # (the house fixture the retired sync-client suite originated).
      target_sources(testsecretagent PRIVATE test_streams_client.cpp)
      # The ported headers reach <cbor.h> through Buffer/buffer.h, whose
      # include dirs are PRIVATE on the library (same libcbor dirs the root's
      # WaveDB block gives secretagent); the test TU needs them directly.
      target_include_directories(testsecretagent PRIVATE
        ${WDB_ROOT}/deps/libcbor/src
        ${WDB_BUILD}/deps/libcbor
        ${WDB_BUILD}/deps/libcbor/src
        ${WDB_ROOT}/deps/hashmap/include)
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
        # TestLoop runs real cells (the loop drives the frame's own pyrt), and
        # TestPyAgent's harness answers bridge requests by hand — both carry
        # the same CPython-owned suppression classes (TestPyrt predates them).
        # TestLiveLoop runs the opt-in live gate (real cells through the
        # frame's own pyrt) when SA_TEST_OLLAMA_URL is set — same classes.
        # TestFrameTree is the orchestration slice's pooled tree: its spawn
        # cells run through the frame's own pyrt on real pool workers — the
        # same CPython-owned classes. TestFrame.TestInlineParentYieldsAwaiting
        # Children (Task 5's inline yield pin) runs a spawn cell the same way.
        # TestRefine's opt-in live gate (Task 7: refine against a real model)
        # seeds its trajectory with real loop turns — the same classes.
        # TestFrame.TestSecondResumeComposesNothing (the resume-repair slice's
        # idempotency pin) runs a spawn cell in its engine-paused (b) shape —
        # the same classes.
        # TestClientApiHandlers.TestInterruptReachesTheFrame (the client-api
        # handlers' slice) interrupts a POOLED frame's real sleeping pyrt cell
        # — the same classes; so does its socket twin in the transport suite.
        if(_test_name MATCHES "^TestPyrt" OR _test_name MATCHES "^TestLoop" OR
           _test_name MATCHES "^TestLiveLoop" OR _test_name MATCHES "^TestPyAgent" OR
           _test_name MATCHES "^TestFrameTree" OR
           _test_name MATCHES "^TestFrame.TestInlineParentYieldsAwaitingChildren$" OR
           _test_name MATCHES "^TestFrame.TestChildrenYieldTurnEndRidesTheFinishBatch$" OR
           _test_name MATCHES "^TestFrame.TestSecondResumeComposesNothing$" OR
           _test_name MATCHES "^TestClientApiHandlers.TestInterruptReachesTheFrame$" OR
           _test_name MATCHES "^TestClientApiUnix.TestInterruptPostsOverTheSocket$" OR
           _test_name MATCHES "^TestRefine.TestRefineLiveRefineAgainstRealModel$" OR
           # Escalation Task 2: the ask park's three pins run real pyrt cells
           # (turn 1's cell calls actor.ask through the frame's own runtime) —
           # the same CPython-owned classes. The engine-less ask pin (the
           # fourth) runs NO cell and keeps full leak checking; the
           # refused-batch pin (Task 3's registration fix) parks and retries
           # against a real cell too — its sibling's registration was missed
           # at Task 2 and its full-LSan run fails on the same classes.
           _test_name MATCHES "^TestFrame.TestAskParksAndBlocksTheTurn$" OR
           _test_name MATCHES "^TestFrame.TestModelAskDuringPlanStaysPlanAfterTheReply$" OR
           _test_name MATCHES "^TestFrame.TestAskStaleReplyIsDroppedLoud$" OR
           _test_name MATCHES "^TestFrame.TestAskSecondAskInSameTurnRefused$" OR
           _test_name MATCHES "^TestFrame.TestAskReplyRefusedBatchKeepsTheParkedAskStanding$")
          set_property(TEST ${_test_name} APPEND PROPERTY ENVIRONMENT "LSAN_OPTIONS=suppressions=${_sa_lsan_supp}")
        endif()
      endforeach()
      unset(_test_name)
    endif()
endif()
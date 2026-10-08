# Qt's Windows GUI entry point can send default test output to the debugger.
# Keep a text log and surface it to CTest when a test fails.
file(REMOVE "${UCD_TEST_LOG}")
execute_process(COMMAND "${UCD_TEST_EXE}" -o "${UCD_TEST_LOG},txt" RESULT_VARIABLE result)
if (EXISTS "${UCD_TEST_LOG}")
    file(READ "${UCD_TEST_LOG}" log)
else ()
    set(log "No test log was produced (loader failure or crash).")
endif ()
if (NOT result EQUAL 0)
    message(FATAL_ERROR "${log}\nTest exited with ${result}")
endif ()
string(REGEX MATCH "Totals:[^\n]+" totals "${log}")
message(STATUS "${totals}")

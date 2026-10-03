# Каждый пример — отдельная программа и smoke-тест CTest (проверяет сам себя, ненулевой код при ошибке).
# 01 — только журнал; 02 — журнал вместе с шиной EventSystem (адаптер, цель EventLogBus).
add_executable(EventLogExample_01_journal_and_recovery 01_journal_and_recovery.cpp)
target_link_libraries(EventLogExample_01_journal_and_recovery PRIVATE engine::EventLog EventLogOptions)

add_executable(EventLogExample_02_bus_events 02_bus_events.cpp)
target_link_libraries(EventLogExample_02_bus_events PRIVATE engine::EventLogBus EventLogOptions)

if(EVENTLOG_BUILD_TESTS)
    add_test(NAME EventLog.example.01_journal_and_recovery COMMAND EventLogExample_01_journal_and_recovery)
    add_test(NAME EventLog.example.02_bus_events COMMAND EventLogExample_02_bus_events)
    set_tests_properties(EventLog.example.01_journal_and_recovery EventLog.example.02_bus_events PROPERTIES LABELS "example")
endif()

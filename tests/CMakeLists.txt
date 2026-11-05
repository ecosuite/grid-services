add_executable(grid_services_tests
        main.cpp
        support/cactus_fixture.cpp
        support/csipaus_fixtures.cpp
        support/fake_adapter.cpp
        support/loopback_server.cpp
        support/memory_store.cpp
        support/oadr_fixtures.cpp
        support/scripted_http.cpp
        support/scripted_rest.cpp
        unit/csipaus_adapter_test.cpp
        unit/csipaus_cactus_test.cpp
        unit/csipaus_factory_test.cpp
        unit/csipaus_identity_test.cpp
        unit/csipaus_limits_test.cpp
        unit/csipaus_reporter_test.cpp
        unit/csipaus_reporting_test.cpp
        unit/csipaus_sep_client_test.cpp
        unit/curl_transport_test.cpp
        unit/datum_test.cpp
        unit/event_sink_test.cpp
        unit/http_client_test.cpp
        unit/normalize_test.cpp
        unit/openadr2b_adapter_test.cpp
        unit/openadr2b_factory_test.cpp
        unit/runner_test.cpp
        unit/sealed_box_test.cpp
        unit/snws2_test.cpp
        unit/supervisor_test.cpp
        unit/types_test.cpp
        integration/pg_store_test.cpp)
target_include_directories(grid_services_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(grid_services_tests PRIVATE
        grid_services_core grid_services_store grid_services_supervisor grid_services_openadr2b
        grid_services_crypto grid_services_csipaus grid_services_http grid_services_solarnetwork
        GTest::gtest)
target_compile_definitions(grid_services_tests PRIVATE
        GRID_SERVICES_MIGRATIONS_DIR="${PROJECT_SOURCE_DIR}/db/migrations"
        GRID_SERVICES_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
target_compile_options(grid_services_tests PRIVATE ${GRID_SERVICES_WARNINGS})
add_test(NAME grid_services COMMAND grid_services_tests)

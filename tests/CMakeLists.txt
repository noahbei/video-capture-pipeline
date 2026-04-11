cmake_minimum_required(VERSION 3.20)

# Helper macro: add a test executable linked against vcpcore
macro(add_vcp_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE vcpcore)
    target_compile_options(${name} PRIVATE -Wall -Wextra)
    add_test(NAME ${name} COMMAND ${name})
endmacro()

# Unit tests — no hardware, no root required
add_vcp_test(test_encryptor)
add_vcp_test(test_config)

# Integration tests — require elevated privileges or hardware
# Run with: ctest -L integration
add_vcp_test(test_disk_monitor)
set_tests_properties(test_disk_monitor PROPERTIES LABELS "integration")

add_vcp_test(test_pipeline_backpressure)
set_tests_properties(test_pipeline_backpressure PROPERTIES LABELS "integration")

add_vcp_test(test_pipeline_reconnect)
set_tests_properties(test_pipeline_reconnect PROPERTIES LABELS "integration")


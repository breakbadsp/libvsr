find_package(GTest REQUIRED)

add_executable(vsr_tests
  checksum_test.cpp
  message_test.cpp
  client_core_test.cpp
  client_table_test.cpp
  io_test.cpp
  journal_test.cpp
  replica_test.cpp
  simulation_test.cpp
  superblock_test.cpp)
target_link_libraries(vsr_tests PRIVATE vsr vsr_io vsr_sim_lib vsr_options GTest::gtest GTest::gtest_main)
add_test(NAME vsr_tests COMMAND vsr_tests)

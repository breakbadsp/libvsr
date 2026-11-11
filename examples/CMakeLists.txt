add_executable(kv_server kv_server.cpp)
target_link_libraries(kv_server PRIVATE vsr_io vsr_options)

add_executable(kv_client kv_client.cpp)
target_link_libraries(kv_client PRIVATE vsr_io vsr_options)

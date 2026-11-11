add_library(vsr_sim_lib simulation.cpp)
target_include_directories(vsr_sim_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(vsr_sim_lib PUBLIC vsr PRIVATE vsr_options)

add_executable(vsr_sim main.cpp)
target_link_libraries(vsr_sim PRIVATE vsr_sim_lib vsr_options)

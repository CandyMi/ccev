add_executable(echo_server echo_server.c)
target_link_libraries(echo_server PRIVATE ccev)

add_executable(timer_demo timer_demo.c)
target_link_libraries(timer_demo PRIVATE ccev)

add_executable(http_bench http_bench.c)
target_link_libraries(http_bench PRIVATE ccev)

add_executable(http1_bench http1_bench.c)
target_link_libraries(http1_bench PRIVATE ccev)

add_executable(example_raw example_raw.c)
target_link_libraries(example_raw PRIVATE ccev)

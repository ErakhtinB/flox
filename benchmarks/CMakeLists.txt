find_package(benchmark REQUIRED)

function(add_flox_benchmark name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE ${PROJECT_NAME} benchmark::benchmark pthread)
    target_compile_options(${name} PRIVATE -O3 -march=native -flto)
    target_compile_definitions(${name} PRIVATE BENCHMARK_ENABLE_AGGREGATES)

endfunction()

add_flox_benchmark(windowed_order_book_benchmark)
add_flox_benchmark(full_order_book_benchmark)
add_flox_benchmark(lockfree_order_book_benchmark)
add_flox_benchmark(book_side_benchmark)
add_flox_benchmark(book_side_latency_benchmark)
add_flox_benchmark(position_manager_benchmark)
add_flox_benchmark(candle_aggregator_benchmark)

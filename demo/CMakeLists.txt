add_executable(flox_demo
    src/main.cpp
    src/demo_connector.cpp
    src/demo_strategy.cpp
    src/demo_builder.cpp
)

target_include_directories(flox_demo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include
                                            ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(flox_demo PRIVATE ${FLOX})

# Error handling demo executable
add_executable(flox_error_demo
    src/error_handling_demo.cpp
)

target_include_directories(flox_error_demo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include
                                                   ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(flox_error_demo PRIVATE ${FLOX})

include_directories(../src/
                    ../salticidae/include/
                    ../
                    ../../Auncel/include/)

add_executable(test_secp256k1 test_secp256k1.cpp)
target_link_libraries(test_secp256k1 hotstuff_static)

add_executable(test_sorting_benchmark ../test_sorting_benchmark.cpp)
target_link_libraries(test_sorting_benchmark hotstuff_static)

add_executable(test_sorting_3way ../test_sorting_3way.cpp)
target_link_libraries(test_sorting_3way hotstuff_static)

add_executable(phi_sweep_bench ../phi_sweep_bench.cpp)
target_link_libraries(phi_sweep_bench hotstuff_static)

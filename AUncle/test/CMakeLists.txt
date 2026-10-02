include_directories(../src/
                    ../salticidae/include/
                    ../)

add_executable(test_secp256k1 test_secp256k1.cpp)
target_link_libraries(test_secp256k1 hotstuff_static)

add_executable(test_weight_orderer test_weight_orderer.cpp)
target_link_libraries(test_weight_orderer hotstuff_static)

add_executable(test_auncel_integration test_auncel_integration.cpp)
target_link_libraries(test_auncel_integration hotstuff_static)

add_executable(test_open test_open.c)
target_link_libraries(test_open PRIVATE ${BINK_LINK_TARGET})
add_test(NAME open COMMAND test_open)

add_executable(test_container test_container.c)
target_link_libraries(test_container PRIVATE ${BINK_LINK_TARGET})
add_test(NAME container COMMAND test_container)

add_executable(test_frames test_frames.c)
target_link_libraries(test_frames PRIVATE ${BINK_LINK_TARGET})
add_test(NAME frames COMMAND test_frames)

add_executable(test_bits test_bits.c)
target_link_libraries(test_bits PRIVATE ${BINK_LINK_TARGET})
add_test(NAME bits COMMAND test_bits)

add_executable(test_dsp test_dsp.c)
target_link_libraries(test_dsp PRIVATE ${BINK_LINK_TARGET})
add_test(NAME dsp COMMAND test_dsp)

add_executable(test_bundles test_bundles.c)
target_link_libraries(test_bundles PRIVATE ${BINK_LINK_TARGET})
add_test(NAME bundles COMMAND test_bundles)

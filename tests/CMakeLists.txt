add_executable(test_open test_open.c)
target_link_libraries(test_open PRIVATE ${BINK_LINK_TARGET})
add_test(NAME open COMMAND test_open)

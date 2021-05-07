project(simple-kanban)

include(TestBigEndian)
test_big_endian(isBigEndian)
if (${isBigEndian})
    set ( WORDS_BIGENDIAN 1 )
endif()

#add_definitions(-DLISTEN_ON_IPV6)

add_executable(simple-kanban server.c)
add_executable(simple-kanban-client client.c)

target_link_libraries(simple-kanban pthread)
target_link_libraries(simple-kanban-client pthread)

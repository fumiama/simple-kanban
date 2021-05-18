cmake_minimum_required(VERSION 3.0.0)
project(simple-kanban)

include(TestBigEndian)
test_big_endian(isBigEndian)
if (${isBigEndian})
    add_definitions(-DWORDS_BIGENDIAN)
endif()

add_definitions(-DLISTEN_ON_IPV6)

add_executable(simple-kanban server.c)
add_executable(simple-kanban-client client.c)
add_executable(cfgwriter cfgwriter.c)

target_link_libraries(simple-kanban spb pthread)
target_link_libraries(simple-kanban-client pthread)
target_link_libraries(cfgwriter spb)

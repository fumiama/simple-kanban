cmake_minimum_required(VERSION 2.6)
project(simple-kanban C)
SET(CMAKE_BUILD_TYPE "Release")

include(TestBigEndian)
test_big_endian(isBigEndian)
if (${isBigEndian})
    add_definitions(-DWORDS_BIGENDIAN)
endif()

add_definitions(-DLISTEN_ON_IPV6)

include_directories("/usr/local/include")
link_directories("/usr/local/lib")

add_executable(simple-kanban server.c)
add_executable(simple-kanban-client client.c)
add_executable(cfgwriter cfgwriter.c)

target_link_libraries(simple-kanban libspb.a pthread)
target_link_libraries(simple-kanban-client pthread)
target_link_libraries(cfgwriter libspb.a)

INSTALL(TARGETS simple-kanban RUNTIME DESTINATION bin)
INSTALL(TARGETS simple-kanban-client RUNTIME DESTINATION bin)

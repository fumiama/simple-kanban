project(simple-kanban)
add_executable(simple-kanban server.c) # 生成可执行文件
add_executable(simple-kanban-client client.c) # 生成可执行文件
target_link_libraries(  # 目标库
                        simple-kanban
 
                        # 目标库需要链接的库
                        pthread)
target_link_libraries(  # 目标库
                        simple-kanban-client
 
                        # 目标库需要链接的库
                        pthread)

add_executable(burstmerge_cli main.cpp)
target_link_libraries(burstmerge_cli PRIVATE burstmerge)
target_include_directories(burstmerge_cli PRIVATE ${CXXOPTS_INCLUDE})

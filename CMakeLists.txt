cmake_minimum_required(VERSION 3.7)

project(monitor
		LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)

add_executable(monitor "monitor.manifest" "monitor.c")
target_link_libraries(monitor PRIVATE "dwmapi")

install(TARGETS monitor DESTINATION .)
install(FILES init.lua DESTINATION .)
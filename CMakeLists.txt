cmake_minimum_required(VERSION 3.5)
project(trimcheck_linux LANGUAGES C)
add_executable(trimcheck trimcheck.c)
target_link_libraries(trimcheck z)
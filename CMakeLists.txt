cmake_minimum_required(VERSION 3.20)
project(MyCppProject LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(.)

add_executable(MyCppProject WIN32
        main.cpp
        datastruct.cpp
        datastruct.h)

target_link_libraries(MyCppProject PRIVATE user32 gdi32 comctl32)

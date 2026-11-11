cmake_minimum_required(VERSION 3.20)
project(MyCppProject LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(.)

add_executable(MyCppProject WIN32
        main.cpp
        datastruct.cpp
        datastruct.h
        password_hasher.cpp
        password_hasher.h
        third_party/sqlite/sqlite3.c
        third_party/sqlite/sqlite3.h)

target_include_directories(MyCppProject PRIVATE third_party/sqlite)

# bcrypt 的随机盐来自 Windows 系统密码学随机数（BCryptGenRandom）。
target_link_libraries(MyCppProject PRIVATE user32 gdi32 comctl32 bcrypt)

# 可选：密码加密自测程序（bcrypt 标准向量 + 数据层集成测试，不需要图形界面）。
# 配置时加 -DBUILD_TESTS=ON，然后构建 password_encryption_test 目标并运行。
option(BUILD_TESTS "Build password encryption self-test" OFF)
if(BUILD_TESTS)
    add_executable(password_encryption_test
            tests/password_encryption_test.cpp
            datastruct.cpp
            password_hasher.cpp
            third_party/sqlite/sqlite3.c)
    target_include_directories(password_encryption_test PRIVATE third_party/sqlite)
    target_link_libraries(password_encryption_test PRIVATE bcrypt)
endif()


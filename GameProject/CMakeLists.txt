cmake_minimum_required(VERSION 3.10)
project(GameProject CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ===== include 目录 =====
include_directories(include)

# 递归搜索 src/ 中的所有 cpp
file(GLOB_RECURSE SRC_FILES
    "src/*.cpp"
)

# 单独加入 main.cpp（因为它不在 src 下面）
set(SRC_FILES ${SRC_FILES} main.cpp)

# 生成可执行文件
add_executable(GameProject ${SRC_FILES})

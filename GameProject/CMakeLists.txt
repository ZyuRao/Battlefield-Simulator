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

add_executable(GameProject ${SRC_FILES} main.cpp)
# 单独加入 main.cpp（因为它不在 src 下面）

# 头文件路径
target_include_directories(GameProject
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

set(SFML_STATIC_LIBRARIES ON)
find_package(SFML 3 REQUIRED COMPONENTS Graphics Window System Audio)

target_link_libraries(GameProject
    PRIVATE
        SFML::Graphics
        SFML::Window
        SFML::System
        SFML::Audio
)
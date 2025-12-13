cmake_minimum_required(VERSION 3.10)
project(GameProject CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)


set(SFML_ROOT "C:/MinGW/mingw64/include/SFML-3.0.2")
# ===== include 目录 =====
include_directories(
    "${CMAKE_SOURCE_DIR}/include"
    "${SFML_ROOT}/include"
)

# 递归搜索 src/ 中的所有 cpp
file(GLOB_RECURSE SRC_FILES
    "src/*.cpp"
)

add_executable(GameProject ${SRC_FILES} main.cpp)
# 单独加入 main.cpp（因为它不在 src 下面）

<<<<<<< HEAD
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
=======
# 生成可执行文件
add_executable(GameProject ${SRC_FILES})

link_directories("${SFML_ROOT}/lib")

target_link_libraries(GameProject
    PRIVATE
        sfml-graphics-3
        sfml-window-3
        sfml-system-3
)
>>>>>>> d5a5e057f448dec79b41fbf5939f952cffdeb01e

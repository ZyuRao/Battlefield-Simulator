cmake_minimum_required(VERSION 3.16)
project(GameProject CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 源文件
file(GLOB_RECURSE SRC_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

add_executable(GameProject
    ${SRC_FILES}
    ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
)


# 头文件路径（包括 FastNoiseLite）
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


# cgltf v1.14 (MIT) — однофайловый парсер glTF 2.0 на C99: https://github.com/jkuhlmann/cgltf
# Файл cgltf.h лежит в репозитории без изменений (sha256 19db3bdf…c64); реализацию подключает ровно одна
# единица трансляции (CGLTF_IMPLEMENTATION) — в AssetSystem, и только там.
cmake_minimum_required(VERSION 3.10)
project(cgltf LANGUAGES C CXX)

add_library(cgltf INTERFACE)
add_library(cgltf::cgltf ALIAS cgltf)
target_include_directories(cgltf INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)

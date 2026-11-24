cmake_minimum_required(VERSION 3.21)
project(DAOQuickSaves LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# DAOLib en sous-module git :
#   git submodule add https://github.com/Whocares66-Dev/DAOLib extern/DAOLib
add_subdirectory(extern/DAOLib EXCLUDE_FROM_ALL)

add_daolib_plugin(daoquicksaves SOURCES src/Plugin.cpp)

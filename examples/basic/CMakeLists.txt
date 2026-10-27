cmake_minimum_required(VERSION 3.22)

# The library is two folders up (repo root)
set(EXTRA_COMPONENT_DIRS "../..")
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(easywifi_basic_example)
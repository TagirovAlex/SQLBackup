cmake_minimum_required(VERSION 3.15)
project(SQLBackup VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded" CACHE STRING "MSVC runtime library")

add_definitions(-DWIN32_LEAN_AND_MEAN -D_WIN32_WINNT=0x0601 -DNOMINMAX)

add_executable(SQLBackup
    src/main.cpp
    src/Config.h
    src/Config.cpp
    src/Logger.h
    src/Logger.cpp
    src/FileUtils.h
    src/FileUtils.cpp
    src/Mailer.h
    src/Mailer.cpp
)

target_link_libraries(SQLBackup PRIVATE
    ws2_32
    advapi32
)

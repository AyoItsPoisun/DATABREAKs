
cmake_minimum_required(VERSION 3.20)

project(DATABREAKs VERSION 0.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(databreaks_audio_test
    tests/audio/audio_test.cpp
)

add_executable(databreaks_engine_003
    tests/audio/engine_003.cpp
)

foreach(target
    databreaks_audio_test
    databreaks_engine_003
)
    target_compile_options(${target} PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
endforeach()

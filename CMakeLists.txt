
cmake_minimum_required(VERSION 3.20)

project(DATABREAKs VERSION 0.15.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(
    ${PROJECT_SOURCE_DIR}/core/audio
    ${PROJECT_SOURCE_DIR}/instruments/synthesizers
)

add_executable(databreaks_audio_test tests/audio/audio_test.cpp)

add_executable(databreaks_engine_003 tests/audio/engine_003.cpp)
add_executable(databreaks_engine_004 tests/audio/engine_004.cpp)
add_executable(databreaks_engine_005 tests/audio/engine_005.cpp)
add_executable(databreaks_engine_006 tests/audio/engine_006.cpp)
add_executable(databreaks_engine_007 tests/audio/engine_007.cpp)
add_executable(databreaks_engine_008 tests/audio/engine_008.cpp)
add_executable(databreaks_engine_009 tests/audio/engine_009.cpp)
add_executable(databreaks_engine_010 tests/audio/engine_010.cpp)
add_executable(databreaks_engine_011 tests/audio/engine_011.cpp)
add_executable(databreaks_engine_012 tests/audio/engine_012.cpp)
add_executable(databreaks_engine_013 tests/audio/engine_013.cpp)
add_executable(databreaks_engine_014 tests/audio/engine_014.cpp)
add_executable(databreaks_engine_015 tests/audio/engine_015.cpp)

add_executable(databreaks_tape_test
    tests/audio/tape_machine_test.cpp
)

add_executable(databreaks_transport_test
    tests/audio/tape_transport_test.cpp
)

add_executable(databreaks_tape_character_test
    tests/audio/tape_character_test.cpp
)

foreach(target
    databreaks_audio_test
    databreaks_engine_003
    databreaks_engine_004
    databreaks_engine_005
    databreaks_engine_006
    databreaks_engine_007
    databreaks_engine_008
    databreaks_engine_009
    databreaks_engine_010
    databreaks_engine_011
    databreaks_engine_012
    databreaks_engine_013
    databreaks_engine_014
    databreaks_engine_015
    databreaks_tape_test
    databreaks_transport_test
    databreaks_tape_character_test
)
    target_compile_options(${target} PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
endforeach()

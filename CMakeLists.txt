
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
add_executable(databreaks_engine_016 tests/audio/engine_016.cpp)
add_executable(databreaks_engine_017 tests/audio/engine_017.cpp)
add_executable(databreaks_engine_018 tests/audio/engine_018.cpp)
add_executable(databreaks_engine_019 tests/audio/engine_019.cpp)
add_executable(databreaks_engine_020 tests/audio/engine_020.cpp)
add_executable(databreaks_engine_021 tests/audio/engine_021.cpp)
add_executable(databreaks_engine_021_render tests/audio/engine_021_render.cpp)
add_executable(databreaks_engine_022 tests/audio/engine_022.cpp)
add_executable(databreaks_engine_022_render tests/audio/engine_022_render.cpp)
add_executable(databreaks_engine_023 tests/audio/engine_023.cpp)
add_executable(databreaks_engine_023_render tests/audio/engine_023_render.cpp)
add_executable(databreaks_engine_024 tests/audio/engine_024.cpp)
add_executable(databreaks_engine_024_render tests/audio/engine_024_render.cpp)
add_executable(databreaks_engine_025 tests/audio/engine_025.cpp)
add_executable(databreaks_engine_025_render tests/audio/engine_025_render.cpp)
add_executable(databreaks_engine_026 tests/audio/engine_026.cpp)
add_executable(databreaks_engine_026_render tests/audio/engine_026_render.cpp)
add_executable(databreaks_engine_027 tests/audio/engine_027.cpp)
add_executable(databreaks_engine_027_render tests/audio/engine_027_render.cpp)
add_executable(databreaks_engine_028 tests/audio/engine_028.cpp)
add_executable(databreaks_engine_028_render tests/audio/engine_028_render.cpp)

# Engine 023 host benchmark: reports measurements only; not a CTest test.
add_executable(databreaks_engine_023_benchmark
    tests/audio/engine_023_benchmark.cpp
)

# Engine 024 host benchmark: reports measurements only; not a CTest test.
add_executable(databreaks_engine_024_benchmark
    tests/audio/engine_024_benchmark.cpp
)

# Engine 025 host benchmark: reports measurements only; not a CTest test.
add_executable(databreaks_engine_025_benchmark
    tests/audio/engine_025_benchmark.cpp
)

# Engine 026 host benchmark: reports measurements only; not a CTest test.
add_executable(databreaks_engine_026_benchmark
    tests/audio/engine_026_benchmark.cpp
)

# Engine 027 host benchmark: reports measurements only; not a CTest test.
add_executable(databreaks_engine_027_benchmark
    tests/audio/engine_027_benchmark.cpp
)

# Engine 028 host benchmark: reports measurements only; not a CTest test.
add_executable(databreaks_engine_028_benchmark
    tests/audio/engine_028_benchmark.cpp
)

# Engine 020 host benchmark: reports measurements only; not a CTest test.
add_executable(databreaks_engine_020_benchmark
    tests/audio/engine_020_benchmark.cpp
)

# engine_020 runs a real producer thread against the event queue.
find_package(Threads REQUIRED)
target_link_libraries(databreaks_engine_020 PRIVATE Threads::Threads)
target_link_libraries(databreaks_engine_022 PRIVATE Threads::Threads)
target_link_libraries(databreaks_engine_025 PRIVATE Threads::Threads)
target_link_libraries(databreaks_engine_026 PRIVATE Threads::Threads)
# engine_028 also interposes pthread_mutex_lock (dlsym) to count locks.
target_link_libraries(databreaks_engine_028 PRIVATE Threads::Threads ${CMAKE_DL_LIBS})

# Assertion-based host unit tests for the synth/DSP headers.
add_executable(databreaks_dsp_unit_tests tests/unit/dsp_unit_tests.cpp)

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
    databreaks_engine_016
    databreaks_engine_017
    databreaks_engine_018
    databreaks_engine_019
    databreaks_engine_020
    databreaks_engine_020_benchmark
    databreaks_engine_021
    databreaks_engine_021_render
    databreaks_engine_022
    databreaks_engine_022_render
    databreaks_engine_023
    databreaks_engine_023_render
    databreaks_engine_023_benchmark
    databreaks_engine_024
    databreaks_engine_024_render
    databreaks_engine_024_benchmark
    databreaks_engine_025
    databreaks_engine_025_render
    databreaks_engine_025_benchmark
    databreaks_engine_026
    databreaks_engine_026_render
    databreaks_engine_026_benchmark
    databreaks_engine_027
    databreaks_engine_027_render
    databreaks_engine_027_benchmark
    databreaks_engine_028
    databreaks_engine_028_render
    databreaks_engine_028_benchmark
    databreaks_dsp_unit_tests
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

# ---------------------------------------------------------------------------
# CTest
#
# Label "unit": programs that check results and exit non-zero on failure.
# Label "render": demos/benchmarks that only prove the program runs to
# completion; they write WAV files into the build directory and make no
# assertions about the audio.
#
#   ctest --test-dir build --output-on-failure          # everything
#   ctest --test-dir build -L unit --output-on-failure  # assertions only
# ---------------------------------------------------------------------------
enable_testing()

foreach(name
    dsp_unit_tests
    engine_011
    engine_012
    engine_013
    engine_015
    engine_016
    engine_017
    engine_018
    engine_019
    engine_020
    engine_021
    engine_022
    engine_023
    engine_024
    engine_025
    engine_026
    engine_027
    engine_028
    tape_test
    transport_test
    tape_character_test
)
    add_test(NAME ${name} COMMAND databreaks_${name})
    set_tests_properties(${name} PROPERTIES LABELS unit)
endforeach()

foreach(name
    audio_test
    engine_003
    engine_004
    engine_005
    engine_006
    engine_007
    engine_008
    engine_009
    engine_010
    engine_014
    engine_021_render
    engine_022_render
    engine_023_render
    engine_024_render
    engine_025_render
    engine_026_render
    engine_027_render
    engine_028_render
)
    add_test(NAME render_${name} COMMAND databreaks_${name})
    set_tests_properties(render_${name} PROPERTIES LABELS render)
endforeach()

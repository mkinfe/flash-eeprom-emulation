cmake_minimum_required(VERSION 3.16)
project(flash_eeprom_emulation VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(EE_BUILD_TESTS "Build the host tests" ON)
option(EE_SANITIZE "Build with address and undefined-behaviour sanitizers" OFF)

function(ee_warnings target)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wcast-qual
            -Wstrict-prototypes -Wmissing-prototypes -Werror)
        if(EE_SANITIZE)
            target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
            target_link_options(${target} PRIVATE -fsanitize=address,undefined)
        endif()
    elseif(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX)
    endif()
endfunction()

add_library(eeprom_emul src/eeprom_emul.c src/ee_crc.c)
target_include_directories(eeprom_emul PUBLIC include PRIVATE src)
ee_warnings(eeprom_emul)

if(EE_BUILD_TESTS)
    enable_testing()

    add_library(sim_flash STATIC sim/sim_flash.c)
    target_include_directories(sim_flash PUBLIC sim)
    target_link_libraries(sim_flash PUBLIC eeprom_emul)
    ee_warnings(sim_flash)

    add_executable(unit_tests tests/unit_tests.c)
    target_link_libraries(unit_tests PRIVATE sim_flash)
    ee_warnings(unit_tests)

    add_executable(powercut_stress tests/powercut_stress.c)
    target_link_libraries(powercut_stress PRIVATE sim_flash)
    ee_warnings(powercut_stress)

    add_test(NAME unit_tests COMMAND unit_tests)
    add_test(NAME powercut_stress_seed1 COMMAND powercut_stress 100000 1)
    add_test(NAME powercut_stress_seed2 COMMAND powercut_stress 100000 2)
    add_test(NAME powercut_stress_seed3 COMMAND powercut_stress 100000 3)
endif()

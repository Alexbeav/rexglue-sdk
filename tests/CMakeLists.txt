# ReXGlue Tests
# Test infrastructure for PPC instruction validation and unit tests

# Unit tests for core components
add_subdirectory(unit)

# PPC instruction tests (requires PPC toolchain)
add_subdirectory(ppc)

# Bounded command-handler regression without a GPU or game data.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME gpu.wait_reg_mem
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/graphics/wait_reg_mem_test.py"
        --compiler "${CMAKE_CXX_COMPILER}"
        --build-dir "${CMAKE_CURRENT_BINARY_DIR}/graphics/$<CONFIG>"
)
set_tests_properties(gpu.wait_reg_mem PROPERTIES TIMEOUT 90 LABELS "gpu;headless")

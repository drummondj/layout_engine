# The `coverage` target's body (CMakeLists.txt): runs the whole ctest suite
# with every instrumented binary writing raw profiles into COVERAGE_DIR/raw,
# merges them, and reports over every binary in COVERAGE_DIR/binaries.txt.
# Inputs: BUILD_DIR, COVERAGE_DIR, CTEST, LLVM_PROFDATA, LLVM_COV, IGNORE_REGEX.

file(REMOVE_RECURSE ${COVERAGE_DIR}/raw)
file(MAKE_DIRECTORY ${COVERAGE_DIR}/raw)
# %4m: up to four pool files per binary that every process merges into, not
# one file per process (1000+ test processes, each a full counter set).
set(ENV{LLVM_PROFILE_FILE} "${COVERAGE_DIR}/raw/%4m.profraw")
execute_process(COMMAND ${CTEST} --test-dir ${BUILD_DIR} -j4 RESULT_VARIABLE tests_failed)
if(tests_failed)
    message(WARNING "coverage: some tests failed (see ctest's output above); reporting what ran")
endif()

file(GLOB raw_profiles ${COVERAGE_DIR}/raw/*.profraw)
if(NOT raw_profiles)
    message(FATAL_ERROR "coverage: no raw profiles in ${COVERAGE_DIR}/raw - is the tree configured with ENABLE_COVERAGE=ON?")
endif()
execute_process(COMMAND ${LLVM_PROFDATA} merge -sparse ${raw_profiles} -o ${COVERAGE_DIR}/coverage.profdata
    COMMAND_ERROR_IS_FATAL ANY)

file(STRINGS ${COVERAGE_DIR}/binaries.txt binaries)
set(objects "")
foreach(binary IN LISTS binaries)
    if(EXISTS ${binary})
        list(APPEND objects -object ${binary})
    endif()
endforeach()
list(POP_FRONT objects) # llvm-cov takes the first binary without -object

set(common -instr-profile=${COVERAGE_DIR}/coverage.profdata --ignore-filename-regex=${IGNORE_REGEX})
execute_process(COMMAND ${LLVM_COV} report ${objects} ${common} --show-branch-summary
    OUTPUT_FILE ${COVERAGE_DIR}/report.txt COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND ${LLVM_COV} export ${objects} ${common} -format=lcov
    OUTPUT_FILE ${COVERAGE_DIR}/lcov.info COMMAND_ERROR_IS_FATAL ANY)
file(READ ${COVERAGE_DIR}/report.txt report)
message("${report}")
message("coverage: report in ${COVERAGE_DIR}/report.txt, lcov data in ${COVERAGE_DIR}/lcov.info")

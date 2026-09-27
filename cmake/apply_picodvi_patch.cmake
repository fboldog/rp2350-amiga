execute_process(
    COMMAND "${GIT_EXECUTABLE}" apply --unidiff-zero --reverse --check
            "${PATCH_FILE}"
    WORKING_DIRECTORY "${SOURCE_DIR}"
    RESULT_VARIABLE patch_already_applied
    OUTPUT_QUIET
    ERROR_QUIET
)

if(NOT patch_already_applied EQUAL 0)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" apply --unidiff-zero --whitespace=nowarn
                "${PATCH_FILE}"
        WORKING_DIRECTORY "${SOURCE_DIR}"
        RESULT_VARIABLE patch_result
    )
    if(NOT patch_result EQUAL 0)
        message(FATAL_ERROR "Unable to apply the RP2350B PicoDVI patch")
    endif()
endif()

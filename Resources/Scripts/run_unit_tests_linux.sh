#!/bin/bash

# Use first argument as TEST_DIR if provided, otherwise use default
TEST_DIR="${1:-../../Build/TestBin}"

# Track overall exit code
EXIT_CODE=0

# Find all executable files that are not .so files
for test_exec in $(find "$TEST_DIR" -type f -executable ! -name "*.so"); do
    echo "Running test: $test_exec"
    "$test_exec"
    TEST_RESULT=$?
    if [ $TEST_RESULT -ne 0 ]; then
        EXIT_CODE=1
    fi
    echo "----------------------------------------"
done

exit $EXIT_CODE
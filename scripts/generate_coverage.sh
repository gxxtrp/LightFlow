#!/usr/bin/env bash
# ==============================================================================
# scripts/generate_coverage.sh
# Automated code coverage generation and verification for LightFlow
# ==============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

BUILD_DIR="${1:-${REPO_ROOT}/build/coverage}"
# Convert BUILD_DIR to absolute path
if [[ ! "${BUILD_DIR}" = /* ]]; then
    BUILD_DIR="${REPO_ROOT}/${BUILD_DIR}"
fi

echo "=============================================================================="
echo " LightFlow Automated Code Coverage Pipeline"
echo " Repo Root : ${REPO_ROOT}"
echo " Build Dir : ${BUILD_DIR}"
echo "=============================================================================="

# ------------------------------------------------------------------------------
# 1. Toolchain Detection
# ------------------------------------------------------------------------------
LLVM_PROFDATA=""
LLVM_COV=""
TOOLCHAIN=""

# Check Apple Clang via xcrun
if command -v xcrun &>/dev/null && xcrun --find llvm-profdata &>/dev/null && xcrun --find llvm-cov &>/dev/null; then
    LLVM_PROFDATA="xcrun llvm-profdata"
    LLVM_COV="xcrun llvm-cov"
    TOOLCHAIN="llvm"
    echo "Detected toolchain: Apple LLVM (via xcrun)"
# Check LLVM in PATH
elif command -v llvm-profdata &>/dev/null && command -v llvm-cov &>/dev/null; then
    LLVM_PROFDATA="llvm-profdata"
    LLVM_COV="llvm-cov"
    TOOLCHAIN="llvm"
    echo "Detected toolchain: LLVM (system PATH)"
# Check Homebrew LLVM on macOS
elif [[ -d "/opt/homebrew/opt/llvm/bin" ]] && [[ -x "/opt/homebrew/opt/llvm/bin/llvm-profdata" ]]; then
    LLVM_PROFDATA="/opt/homebrew/opt/llvm/bin/llvm-profdata"
    LLVM_COV="/opt/homebrew/opt/llvm/bin/llvm-cov"
    TOOLCHAIN="llvm"
    echo "Detected toolchain: Homebrew LLVM (/opt/homebrew/opt/llvm/bin)"
elif [[ -d "/usr/local/opt/llvm/bin" ]] && [[ -x "/usr/local/opt/llvm/bin/llvm-profdata" ]]; then
    LLVM_PROFDATA="/usr/local/opt/llvm/bin/llvm-profdata"
    LLVM_COV="/usr/local/opt/llvm/bin/llvm-cov"
    TOOLCHAIN="llvm"
    echo "Detected toolchain: Homebrew LLVM (/usr/local/opt/llvm/bin)"
# Check GCC / lcov
elif command -v gcov &>/dev/null && command -v lcov &>/dev/null; then
    TOOLCHAIN="gcc"
    echo "Detected toolchain: GNU GCC / lcov"
else
    echo "ERROR: Neither LLVM coverage tools (llvm-profdata, llvm-cov) nor GCC (gcov, lcov) were found." >&2
    exit 1
fi

# ------------------------------------------------------------------------------
# 2. Configure & Build Instrumented Targets
# ------------------------------------------------------------------------------
echo "--- Configuring LightFlow with LF_ENABLE_COVERAGE=ON ---"
cmake -B "${BUILD_DIR}" -S "${REPO_ROOT}" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DLF_ENABLE_COVERAGE=ON \
    -DLF_BUILD_TESTS=ON

echo "--- Building all targets ---"
cmake --build "${BUILD_DIR}" -j

# ------------------------------------------------------------------------------
# 3. Clean Stale Profiles and Execute Test Suite
# ------------------------------------------------------------------------------
PROFILES_DIR="${BUILD_DIR}/profiles"
rm -rf "${PROFILES_DIR}"
mkdir -p "${PROFILES_DIR}"
rm -f "${BUILD_DIR}/coverage.profdata" "${BUILD_DIR}/coverage.info"

echo "--- Executing Test Suite via CTest ---"
if [[ "${TOOLCHAIN}" == "llvm" ]]; then
    export LLVM_PROFILE_FILE="${PROFILES_DIR}/code-%p-%m.profraw"
fi

ctest --test-dir "${BUILD_DIR}" --output-on-failure

# ------------------------------------------------------------------------------
# 4. Process Profile Data and Generate Reports
# ------------------------------------------------------------------------------
HTML_DIR="${BUILD_DIR}/html"
rm -rf "${HTML_DIR}"
mkdir -p "${HTML_DIR}"

if [[ "${TOOLCHAIN}" == "llvm" ]]; then
    echo "--- Merging Raw LLVM Profiles ---"
    # shellcheck disable=SC2086
    ${LLVM_PROFDATA} merge -sparse "${PROFILES_DIR}"/*.profraw -o "${BUILD_DIR}/coverage.profdata"

    # Identify all test executables
    KNOWN_BINS=(
        "lf_task_graph_test"
        "lf_scheduler_test"
        "lf_memory_test"
        "lf_gpu_sync_test"
        "lf_concurrency_stress_test"
        "lf_comparison_test"
        "lf_tracy_test"
    )

    TEST_BINS=()
    for bin_name in "${KNOWN_BINS[@]}"; do
        candidate="${BUILD_DIR}/tests/${bin_name}"
        if [[ -f "${candidate}" && -x "${candidate}" ]]; then
            TEST_BINS+=("${candidate}")
        fi
    done

    # Fallback to dynamic scan if none found in tests/
    if [[ ${#TEST_BINS[@]} -eq 0 ]]; then
        while IFS= read -r f; do
            if [[ -x "$f" && ! "$f" =~ \.dSYM && ! "$f" =~ \.o$ && ! "$f" =~ \.a$ && ! "$f" =~ \.dylib$ ]]; then
                TEST_BINS+=("$f")
            fi
        done < <(find "${BUILD_DIR}/tests" -type f 2>/dev/null)
    fi

    if [[ ${#TEST_BINS[@]} -eq 0 ]]; then
        echo "ERROR: No test executables found in ${BUILD_DIR}/tests." >&2
        exit 1
    fi

    FIRST_BIN="${TEST_BINS[0]}"
    OBJECT_ARGS=()
    for ((i = 1; i < ${#TEST_BINS[@]}; ++i)); do
        OBJECT_ARGS+=("-object=${TEST_BINS[i]}")
    done

    IGNORE_REGEX="(_deps/|tests/|tools/|/usr/|external/|v1/|include/c\+\+/)"

    echo "--- Generating Terminal Summary Report ---"
    REPORT_OUTPUT=$(${LLVM_COV} report "${FIRST_BIN}" "${OBJECT_ARGS[@]}" \
        -instr-profile="${BUILD_DIR}/coverage.profdata" \
        --ignore-filename-regex="${IGNORE_REGEX}" \
        --show-branch-summary \
        --show-region-summary)

    echo "${REPORT_OUTPUT}"

    echo "--- Generating Detailed HTML Report ---"
    ${LLVM_COV} show "${FIRST_BIN}" "${OBJECT_ARGS[@]}" \
        -instr-profile="${BUILD_DIR}/coverage.profdata" \
        -format=html \
        -output-dir="${HTML_DIR}" \
        --ignore-filename-regex="${IGNORE_REGEX}" \
        --show-branch-summary \
        --show-region-summary \
        --project-title="LightFlow Code Coverage"

    echo "HTML report generated: file://${HTML_DIR}/index.html"

    # --------------------------------------------------------------------------
    # 5. Threshold Verification (>90% Line Coverage)
    # --------------------------------------------------------------------------
    echo "--- Evaluating Coverage Thresholds ---"
    TOTAL_LINE=$(echo "${REPORT_OUTPUT}" | grep -E "^TOTAL " || true)
    if [[ -n "${TOTAL_LINE}" ]]; then
        echo "Overall: ${TOTAL_LINE}"
        # Extract percentage numbers ending with %
        PERCENTAGES=($(echo "${TOTAL_LINE}" | grep -oE '[0-9]+\.[0-9]+%'))
        # Typically: [Regions%, Executed%, Lines%, Branches%]
        if [[ ${#PERCENTAGES[@]} -ge 3 ]]; then
            LINE_COVER="${PERCENTAGES[2]}"
            LINE_VAL="${LINE_COVER%\%}"
            echo "Total Line Coverage: ${LINE_COVER}"
            if awk "BEGIN {exit !(${LINE_VAL} >= 90.0)}"; then
                echo "SUCCESS: Line coverage (${LINE_COVER}) meets or exceeds the 90% threshold!"
            else
                echo "WARNING: Line coverage (${LINE_COVER}) is below the 90% target threshold."
            fi
        fi
        if [[ ${#PERCENTAGES[@]} -ge 4 ]]; then
            BRANCH_COVER="${PERCENTAGES[3]}"
            echo "Total Branch Coverage: ${BRANCH_COVER}"
        fi
    fi

elif [[ "${TOOLCHAIN}" == "gcc" ]]; then
    echo "--- Processing Coverage with LCOV ---"
    lcov --capture --directory "${BUILD_DIR}" --output-file "${BUILD_DIR}/coverage.info"
    lcov --remove "${BUILD_DIR}/coverage.info" '*/tests/*' '*/tools/*' '*/_deps/*' '/usr/*' \
        --output-file "${BUILD_DIR}/coverage_filtered.info"

    echo "--- Summary ---"
    lcov --list "${BUILD_DIR}/coverage_filtered.info"

    if command -v genhtml &>/dev/null; then
        echo "--- Generating HTML Report ---"
        genhtml "${BUILD_DIR}/coverage_filtered.info" --output-directory "${HTML_DIR}" \
            --title "LightFlow Code Coverage"
        echo "HTML report generated: file://${HTML_DIR}/index.html"
    fi
fi

echo "=============================================================================="
echo " Coverage Pipeline Complete!"
echo " Report URL: file://${HTML_DIR}/index.html"
echo "=============================================================================="

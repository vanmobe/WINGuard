#!/bin/bash

# Build script for AUDIOLAB.wing.reaper.virtualsoundcheck Reaper Extension
# Supports macOS and Windows (via MSYS/MinGW/Cygwin shell on Windows)

set -e

echo "========================================="
echo "AUDIOLAB.wing.reaper.virtualsoundcheck - Build Script"
echo "========================================="
echo ""

# Detect OS
OS="$(uname -s)"
case "${OS}" in
    Darwin*)    PLATFORM=macOS;;
    MINGW*|MSYS*|CYGWIN*) PLATFORM=Windows;;
    *)          PLATFORM=Unsupported;;
esac

echo "Platform: ${PLATFORM}"
echo ""

if [ "${PLATFORM}" == "Unsupported" ]; then
    echo "Error: unsupported build platform. Use macOS or Windows."
    exit 1
fi

# Configuration
BUILD_TYPE="Release"
WITH_TESTS=0

usage() {
    echo "Usage: $0 [Release|Debug|RelWithDebInfo|MinSizeRel] [--with-tests]"
}

for arg in "$@"; do
    case "$arg" in
        --with-tests)
            WITH_TESTS=1
            ;;
        Release|Debug|RelWithDebInfo|MinSizeRel)
            BUILD_TYPE="$arg"
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Error: unknown argument '$arg'"
            usage
            exit 1
            ;;
    esac
done

BUILD_DIR="build"
if [ "${WITH_TESTS}" -eq 1 ]; then
    BUILD_DIR="build-tests"
fi
INSTALL_DIR="install"

echo "Build type: ${BUILD_TYPE}"
echo "Run tests: $([ "${WITH_TESTS}" -eq 1 ] && echo "yes" || echo "no")"
echo "Build directory: ${BUILD_DIR}"
echo ""

# Create build directory
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

# Check for dependencies
echo "Checking dependencies..."

# Check for CMake
if ! command -v cmake &> /dev/null; then
    echo "Error: CMake not found"
    echo "Please install CMake: https://cmake.org/download/"
    exit 1
fi

CMAKE_VERSION=$(cmake --version | head -n1)
echo "Found ${CMAKE_VERSION}"

# Run CMake
echo ""
echo "Running CMake configuration..."
cmake .. -DCMAKE_BUILD_TYPE=${BUILD_TYPE} \
         -DBUILD_TESTS=$([ "${WITH_TESTS}" -eq 1 ] && echo ON || echo OFF) \
         -DCMAKE_INSTALL_PREFIX=../${INSTALL_DIR}

if [ $? -ne 0 ]; then
    echo "CMake configuration failed"
    exit 1
fi

# Build
echo ""
echo "Building..."
cmake --build . --config ${BUILD_TYPE} -j$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

if [ $? -ne 0 ]; then
    echo "Build failed"
    exit 1
fi

# Install
echo ""
echo "Installing to ${INSTALL_DIR}..."
cmake --install . --config ${BUILD_TYPE}

if [ "${WITH_TESTS}" -eq 1 ]; then
    echo ""
    echo "Running tests..."
    ctest --output-on-failure --build-config ${BUILD_TYPE}
fi

cd ..

# Show results
echo ""
echo "========================================="
echo "Build Complete!"
echo "========================================="
echo ""
echo "Extension built: ${INSTALL_DIR}/reaper_wingconnector${EXTENSION_SUFFIX}"
echo ""
echo "Installation instructions:"
echo ""

if [ "${PLATFORM}" == "macOS" ]; then
    REAPER_PATH="$HOME/Library/Application Support/REAPER/UserPlugins"
    echo "Copy to: ${REAPER_PATH}/"
    echo ""
    echo "Quick install:"
    echo "  mkdir -p \"${REAPER_PATH}\""
    echo "  cp ${INSTALL_DIR}/reaper_wingconnector.dylib \"${REAPER_PATH}/\""
    echo "  cp config.json \"${REAPER_PATH}/\""
elif [ "${PLATFORM}" == "Windows" ]; then
    REAPER_PATH="%APPDATA%\\REAPER\\UserPlugins"
    echo "Copy to: ${REAPER_PATH}\\"
    echo ""
    echo "Quick install:"
    echo "  copy ${INSTALL_DIR}\\reaper_wingconnector.dll \"%APPDATA%\\REAPER\\UserPlugins\\\""
    echo "  copy config.json \"%APPDATA%\\REAPER\\UserPlugins\\\""
fi

echo ""
echo "Then restart Reaper to load the extension."
echo ""

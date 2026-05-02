#!/bin/bash

# Exit immediately if a command exits with a non-zero status
set -e

echo "Building f1_compiler..."

# Create and navigate to the build directory
mkdir -p build
cd build

# Configure and build the project
cmake ..
make -j$(nproc 2>/dev/null || sysctl -n hw.ncpu)

# Copy the binary to the profile folder
# Using find to locate the 'figlut_test' binary regardless of the exact nested CMake output path
BINARY_PATH=$(find . -name "figlut_test" -type f | head -n 1)

cp "$BINARY_PATH" ../../sim/profile/
echo "Build complete. Binary copied to sim/profile/."
#!/bin/bash

echo "=== Test Set 1 (degree = 8192) ==="
./figlut_test 8 128 128 1 | tee test1_a.log
./figlut_test 8 256 256 1 | tee test1_b.log
./figlut_test 8 512 512 1 | tee test1_c.log

echo ""
echo "=== Test Set 2 (degree = 16384) ==="
./figlut_test 16 128 128 1 | tee test2_a.log
./figlut_test 16 256 256 1 | tee test2_b.log
./figlut_test 16 512 512 1 | tee test2_c.log

# echo ""
# echo "=== Test Set 3 (degree = 8192) ==="
# Input: 4096 * (degree * 1), Weight: (2 * 4096 * 4096) 
./figlut_test 8 4096 4096 1 | tee test3_a.log
# # Input: 4096 * (degree * 8), Weight: (2 * 4096 * 4096)
./figlut_test 8 4096 4096 8 | tee test3_b.log
#!/bin/bash

N_MPU=$1

# echo "=== Test Set 1 (degree = 8192) ==="
# ./figlut_test 8 128 128 1 $N_MPU | tee test1_a_$N_MPU.log
# ./figlut_test 8 256 256 1 $N_MPU | tee test1_b_$N_MPU.log
# ./figlut_test 8 512 512 1 $N_MPU | tee test1_c_$N_MPU.log
# ./figlut_test 8 192 192 1 $N_MPU | tee test1_d_$N_MPU.log

# echo ""
# echo "=== Test Set 2 (degree = 16384) ==="
# ./figlut_test 16 128 128 1 $N_MPU | tee test2_a_$N_MPU.log
# ./figlut_test 16 256 256 1 $N_MPU | tee test2_b_$N_MPU.log
# ./figlut_test 16 512 512 1 $N_MPU | tee test2_c_$N_MPU.log

# echo ""
# echo "=== Test Set 3 (degree = 8192) ==="
# Input: 4096 * (degree * 1), Weight: (2 * 4096 * 4096) 
# ./figlut_test 8 4096 4096 1 $N_MPU | tee test3_a.log
# # Input: 4096 * (degree * 8), Weight: (2 * 4096 * 4096)
# ./figlut_test 8 4096 4096 8 $N_MPU | tee test3_b.log

./figlut_test 16 2048 2048 1 $N_MPU | tee test4_a_$N_MPU.log &
./figlut_test 16 4096 4096 1 $N_MPU | tee test4_b_$N_MPU.log &
./figlut_test 16 16384 4096 1 $N_MPU | tee test4_c_$N_MPU.log &
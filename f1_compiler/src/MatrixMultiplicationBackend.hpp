#pragma once

#include "PolynomialRing.hpp"
#include "UINTNode.hpp"
#include "Utils.hpp"
#include "NTT.hpp"

#include <cstdint>

namespace f1 {

struct FIGLUTConfig {
    uint32_t pe_row;
    uint32_t pe_col;
    uint32_t rac_per_pe;
    uint32_t mu;
    uint32_t mpu;
};

class FIGLUTBackend {
public:

    FIGLUTBackend(FIGLUTConfig cfg);

    std::vector<CipherTextNodePtr> run(std::vector<CipherTextNodePtr> input, std::vector<UINT64NodePtr> weight) {

        


        


    } 


private:
    FIGLUTConfig figlut;
};



}




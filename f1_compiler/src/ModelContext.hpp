#pragma once
#include "DAG.hpp"

namespace f1 {

//* forward declaration
class F1TargetMachine;

struct F1ModelContext {
public:

    //* DAG graph
    DAG* dag; 
    const F1TargetMachine* target_machine;
 
};

}

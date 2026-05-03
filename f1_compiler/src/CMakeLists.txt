
add_library(f1_lib
    Inst.cpp
    F1Model.cpp
    ClusterModel.cpp
    MemoryAllocator.cpp
)

target_link_libraries(f1_lib fmt)
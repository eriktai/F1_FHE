# F1 Compiler

The `f1_compiler` is a specialized Intermediate Representation (IR) and Code Generation pipeline tailored for Fully Homomorphic Encryption (FHE) algorithms targeting the F1 hardware architecture.

## Compilation Flow

The compiler lowers high-level FHE algorithms into optimized, hardware-aware instruction schedules through a multi-stage pipeline:

### 1. DAG Construction (Frontend)
- **Operation:** High-level operations (like BCQ Matrix Multiplication, Key Switching) are defined using Backends (e.g., `CipherTextFIGLUT_BCQBackend`, `KeySwitchBackend`).
- **Action:** These backends translate the algorithm into a Directed Acyclic Graph (`DAG`).
- **Components:** Data dependencies are explicitly captured using `DataNode` (e.g., `CipherTextNode`, `KeySwitchKeyNode`) and `OperatorNode` (e.g., `NTT`, `ModMul`, `ModAdd`).

### 2. Code Generation (IR to Instructions)
- **Operation:** The `CodeGenerator` traverses the `DAG` in topological order.
- **Action:** Emits a linear sequence of abstract F1 instructions (`Inst`) that preserve the logical dependencies and algebraic semantics of the original FHE algorithm.

### 3. Spatial Scheduling
- **Operation:** `SpatialScheduler::schedule()`
- **Action:** Distributes the linear sequence of instructions across the available heterogeneous computing clusters.
- **Heuristics:** Optimizes for **Data Temporal Locality** (keeping computation close to where data resides) and inserts network `Load` instructions to move operands when necessary.

### 4. Register Allocation & Liveness Analysis
- **Operation:** `LivenessAnalyzer::compute()` and `RegisterAllocator::allocate()`
- **Action:** Analyzes the lifespan of data variables within each cluster to intelligently allocate and free space in the cluster's Vector Register File.

### 5. Memory Allocation
- **Operation:** `MemoryAllocator::allocate()`
- **Action:** Maps non-register data elements (like large key-switch keys or static weights) into the physical Scratchpad SRAM banks.

### 6. Temporal Scheduling (Cycle-Accurate Simulation)
- **Operation:** `ClusterModel::schedule()`
- **Action:** Acts as a local hardware scheduler inside each cluster.
- **Details:** Employs a Ready-List Scheduling algorithm that tracks the cycle-by-cycle availability of functional units (NTT, MM, MA) and Memory Status Holding Registers (MSHRs). It reorders instructions to minimize structural hazards and pipeline stalls.

### 7. Output & Reporting
- **Action:** Generates detailed compilation reports, including:
  - `schedule.out`: The final allocated instruction sequence.
  - `time_series_schedule.out`: A cycle-by-cycle execution timeline.
  - `memory_allocation.out`: Details of scratchpad bank utilization and memory conflict analysis.
  - `cluster_X_hazards.csv`: Telemetry on pipeline bubbles and backpressure.

## Building the Project

To build the compiler executable and copy it to the simulation profile directory, you can use the included `build.sh` script:
```bash
cd f1_compiler
./build.sh
```
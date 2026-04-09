# F1_FHE

## f1_compiler summary

### Core architecture

f1_compiler is structured as an IR + codegen pipeline for FHE operations:

- DAG.hpp
  - Defines the core IR node system: `Node`, `OperatorNode`, `DataNode`, and `DAG`.
  - `DAG` is the node pool and registry: it allocates nodes, assigns unique IDs, and groups data nodes by `DataType`.
  - This is the compiler’s semantic graph representation before lowering to instructions.

- PolynomialRing.hpp
  - Defines FHE-specific data node types such as `KeySwitchKeyNode`, `CipherTextNode`, `RnsPolyNode`, `CoeffVecNode`, etc.
  - Those nodes hold the algebraic objects used in homomorphic operations.
  - `KeySwitchKeyNode` is the data representation of a key-switch key in the DAG.

- DAGTraversal.hpp
  - Provides a generic visitor/traversal framework over the DAG.
  - `DAGTraversalBase` can recurse into ciphertexts, keyswitch keys, RNS polynomials, etc.
  - `CodeGenerator` and other utilities reuse this traversal to walk the graph.

- Inst.hpp
  - Defines the low-level instruction abstraction (`Inst`), with an opcode enum (`InstId`) and source/destination descriptors.
  - This is the bridge between the DAG and a linear schedule.

- CodeGen.hpp
  - Implements `CodeGenerator` that converts DAG nodes into an instruction sequence.
  - It visits data nodes whose children are operator nodes, then emits `Inst` objects for operations like `NTT`, `INTT`, `ModAdd`, `ModMul`.
  - It preserves DAG dependency order by giving each instruction a sequence ID from the node ID.

---

## Why you need `DAG` class

The `DAG` class is needed because:

- It represents computation as a directed acyclic graph instead of a flat sequence.
- It captures data dependencies explicitly:
  - which ciphertext/relation depends on which keyswitch key fragments,
  - which intermediate polynomial node comes from which operator.
- It allows the compiler to:
  - build FHE computations modularly,
  - reuse intermediate nodes,
  - traverse and generate code in dependency order.
- Without the DAG, you would have a much harder time separating logical FHE semantics from instruction scheduling.

In short: `DAG` is the IR layer that lets you express the algorithm independently of physical instruction layout.

---

## Why you need `KeySwitchKeyBackend`

`KeySwitchKeyBackend` is the FHE-specific lowering pass for key switching:

- It implements the key-switch algorithm on DAG nodes:
  - it takes a ciphertext `ct`,
  - the `c2` polynomial piece,
  - the `ksk` key-switch key,
  - and the modulus vector.
- It constructs the underlying DAG of operations:
  - `INTTOpNode` on `c2`,
  - `NTTOpNode` for cross-modulus multiplication,
  - `ModMulOpNode` and `ModAddOpNode` for accumulation,
  - final addition into the new ciphertext components.

Why this matters:
- Key switching is the core FHE transform that changes ciphertext key material.
- Implementing it in `KeySwitchKeyBackend` isolates the algorithm from scheduling.
- It produces a DAG of low-level arithmetic and transforms, which can then be codegen’d.

So: `KeySwitchKeyBackend` is the backend that turns a high-level FHE key-switch operation into a DAG of primitive ops.

---

## Why you need `F1Model` to generate instruction sequence

`F1Model` is the hardware/software mapping layer:

- It owns the microarchitecture model:
  - number of clusters,
  - scratchpad size/bandwidth,
  - register file config,
  - cluster compute capability.
- `F1Model::compile(DAG* dag, CipherTextNodePtr root_node)` does two things:
  1. Uses `CodeGenerator` to traverse the DAG and emit a list of instructions.
  2. Performs spatial scheduling / cluster allocation:
     - assigns each instruction to a cluster,
     - inserts `Load` instructions for source data,
     - tracks data placement and movement across clusters,
     - writes out a schedule and data-movement report.

Why this is needed:
- The DAG only expresses what to compute, not where or in what order on the target machine.
- `F1Model` converts DAG semantics into a concrete schedule for a cluster-based architecture.
- It also computes the cost of data movement and cluster load balancing.

So: `F1Model` is the stage that turns the DAG into an actual executable instruction sequence for your F1 architecture.

---

## How they fit together

1. Build the DAG representing FHE computation.
2. Use `KeySwitchKeyBackend` to expand key-switch operations into lower-level DAG nodes.
3. Use `F1Model` to traverse that DAG:
   - `CodeGenerator` translates nodes into `Inst` objects.
   - `F1Model` schedules those instructions onto clusters.
   - It emits a concrete instruction sequence and cluster/data movement metrics.

This is the compiler flow: FHE algorithm → DAG IR → instruction generation → hardware-aware schedule.

If you want, I can also summarize the exact dataflow in `KeySwitchKeyBackend::run()` step-by-step.
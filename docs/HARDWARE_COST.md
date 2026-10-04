# Hardware cost of MOESI-D

First-order area, access-energy and leakage estimates for the structures MOESI-D
adds to an L2 bank, from CACTI 7 (HewlettPackard/cacti, master) at 22 nm, ITRS-HP
cells, 350 K, one read/write port, no ECC. The exact configurations and raw
CACTI output are in `results/cacti_22nm/`; `tools/cacti_moesi_d.sh` regenerates
them. CACTI's smallest valid arrays are larger than two of the structures, so
those rows are upper bounds (noted). An L2 bank's own 256 KB data array and a
32 KB L1D are included for scale.

| structure (per L2 bank) | modelled as | area (mm²) | access (ns) | read / write (pJ) | leakage (mW) |
|---|---|---:|---:|---:|---:|
| Admission table, 256 entries | 4 KB, 16 B entries, 4-way | 0.0064 | 0.14 | 3.1 / 3.3 | 1.77 |
| Admission table, 1,024 entries (round 7 size) | 16 KB, 16 B entries, 4-way | 0.0167 | 0.18 | 4.7 / 6.6 | 6.40 |
| Executor queue, 16 × 128 B operands | 4 KB RAM, 128 B words (upper bound; 2 KB is below CACTI's minimum) | 0.0269 | 0.16 | 22.2 / 22.8 | 1.82 |
| Hot-word buffer, 32 × 16 B | 2 KB, 16 B entries, 4-way (upper bound; 512 B is below CACTI's minimum) | 0.0047 | 0.13 | 2.8 / 2.7 | 0.99 |
| Merge record, 64 × 16 B | 1 KB RAM | 0.0013 | 0.08 | 0.7 / 1.1 | 0.38 |
| **Total added per bank** | | **0.039** | | | **4.96** |
| L2 bank data array, 256 KB, 128 B lines, 8-way (for scale) | | 0.814 | 1.37 | 296 / 315 | 71.7 |
| L1D, 32 KB, 64 B lines, 8-way (for scale) | | 0.165 | 0.69 | 65 / 65 | 10.9 |

Reading:

- **Area.** The added structures are 0.039 mm² per bank, 4.8 % of the bank's
  data array at 22 nm, or about 0.16 mm² across the four banks of the evaluated
  configuration. The 32/64-bit adder and its masked multi-word application are
  not modelled; a 64-bit adder at 22 nm is on the order of 10⁻³ mm², below the
  rounding of the table. Nothing is added to the L1 beyond protocol states.
- **Leakage.** About 5 mW per bank against 72 mW for the bank's data array (7 %),
  dominated by the executor queue and admission table.
- **Access energy.** Every structure is accessed in a few picojoules; an executor
  operand read/write is ~22 pJ. A delegated add therefore costs on the order of
  50 pJ of structure energy plus one L2 line access (~300 pJ), against a
  migration that costs an L2 access, an L1 fill (~65 pJ) and a 128-byte line on
  the network. Where delegation cuts injected flits by 6–7× (the many-line
  workloads), it also cuts the dominant energy term; where it raises flits
  (single hot lines, 1.7–2.9×), interconnect energy rises with it.
- **Interconnect energy** is reported as flit counts only (results page). A
  per-flit energy depends on link length and router design; multiplying the
  flit ratios by any single figure would not change the ratios, which are the
  claim.
- **Access time.** All added structures are well inside one L2 cycle at the
  evaluated clock; the executor's 20-cycle service time is a protocol-level
  assumption (results page, round 9), not a CACTI result.

Not covered: physical design of the executor's control, the SLICC transient
states' TBE entries (shared with the base protocol's TBE pool), and package-level
energy. These are the usual limits of a CACTI-level estimate.

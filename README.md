# Mitosis + WASP: NUMA-Aware Page Table Replication for Linux 7.0.0

This repository contains two components for reducing NUMA-induced page table walk latency on multi-socket machines:

**Mitosis** is a Linux 7.0.0 kernel patch that replicates user-space page tables across NUMA nodes so that each node walks a local copy during TLB misses. This is a port of the original Mitosis design to Linux 7.0.0 with corrections, new infrastructure, and runtime control interfaces.

**WASP** (Workload-Aware Self-Replicating Page-Tables) is a userspace daemon that monitors running processes via hardware performance counters (memory access rate and dTLB miss rate) and automatically enables or disables Mitosis replication based on whether a workload would benefit from it. WASP also measures inter-node page table access latency and steers each node's CR3 toward the replica with the lowest latency.

The kernel patch lives in this repository. The WASP daemon lives at: https://github.com/julianw1010/WASP-daemon

## References

Achermann, R., Panwar, A., Bhattacharjee, A., Roscoe, T., and Gandhi, J. (2020). *Mitosis: Transparently Self-Replicating Page-Tables for Large-Memory Machines.* ASPLOS '20, pp. 283-300. [doi:10.1145/3373376.3378468](https://doi.org/10.1145/3373376.3378468)

Wang, J., Wang, Z., Li, Y., Chen, L., and Li, J. (2024). *WASP: Workload-Aware Self-Replicating Page-Tables for NUMA Servers.* ASPLOS '24, Vol. 2, pp. 214-229. [doi:10.1145/3620665.3640369](https://doi.org/10.1145/3620665.3640369)

Original Mitosis implementation (Linux 4.17): https://github.com/julianw1010/mitosis-linux

## How It Works

### Kernel (Mitosis)

When replication is enabled for a process, the kernel allocates a separate copy of its page table tree on each configured NUMA node. All page table writes (set_pte, set_pmd, set_pud, set_p4d, set_pgd) are intercepted through the paravirt_ops interface and broadcast to every replica. On context switch, the kernel loads CR3 from the replica local to the CPU's NUMA node, so subsequent TLB misses walk node-local memory.

Child pointers in replica entries are rewritten to point at the node-local child page table, so the entire walk from PGD down to PTE stays on one node.

### Daemon (WASP)

The WASP daemon (`waspd`) runs as root and periodically scans `/proc` for user processes. For each process it attaches perf counters to measure memory access rate (MAR) and dTLB miss rate. When both metrics exceed configurable thresholds for a sustained hysteresis period, the daemon enables Mitosis replication for that process via `prctl`. When the metrics drop below threshold, it disables replication.

WASP also periodically measures page table access latency between all NUMA node pairs by forking a child process, pinning it to each source node, and timing cache-line-flushed reads to buffers bound to each destination node. The resulting latency matrix determines a steering table: for each source node, WASP selects the replica node with the lowest access latency and pushes the steering configuration to the kernel via `prctl(PR_SET_PGTABLE_REPL_STEERING)`.

## Prerequisites

Tested on Ubuntu 24.04.04 LTS. Run the prepare script to install build dependencies and generate the kernel configuration:

```bash
./prepare.sh
```

This installs build tools and development libraries, copies the running kernel's config, detects the NUMA node count via `numactl --hardware`, sets the required config options, and runs `make olddefconfig`.

## Kernel Configuration

Two config options are required (set automatically by `prepare.sh`):

**PARAVIRT_XXL** must be enabled. Mitosis uses the paravirt_ops hooks to intercept page table writes and broadcast them to replicas.

**MITOSIS_NUMA_NODE_COUNT** must match the number of NUMA nodes on your system (check with `numactl --hardware`). The kernel will refuse to boot on mismatch. This controls the size of per-mm replica pointer arrays and the per-node page table cache.

To adjust manually:

```
make menuconfig
# Processor type and features -> Paravirtualization -> PARAVIRT_XXL: Y
# Processor type and features -> NUMA -> MITOSIS_NUMA_NODE_COUNT: your node count
```

## Building and Installation

### Kernel

```bash
make -j$(nproc)
./install.sh
```

`install.sh` removes any previous `7.0.0-wasp` images from `/boot`, builds the kernel, and installs modules and image. Reboot into the new kernel after installation.

### WASP Daemon

See https://github.com/julianw1010/WASP-daemon for build and usage instructions.

## Usage

### Automatic mode with WASP daemon

The recommended way to use this system is to boot the Mitosis kernel and run the WASP daemon:

```bash
sudo waspd
```

The daemon will monitor all user processes, measure inter-node latencies, and enable or disable replication automatically. Key options:

```
waspd [options]
  -i N    PTL measurement interval in ms (default: 1000)
  -u N    Main loop interval in ms (default: 1000)
  -y N    Hysteresis duration in ms (default: 1000)
  -c N    Pre-populate page table cache with N pages/node
  -n M    Node mask: comma-separated list of nodes, e.g. 0,2,3
  -g      Force generic HW_CACHE events (disable raw PMU events)
```

The daemon provides a live TUI showing the PTL latency matrix, steering decisions, and per-process MAR/dTLB metrics.

### Boot-time replication (without WASP)

For manual use without the daemon, the `mitosis` boot parameter auto-enables replication for all new user processes:

```
# In your bootloader config (e.g. GRUB_CMDLINE_LINUX):
mitosis
```

Replication can also be enabled from the command line using [numactl-wasp](https://github.com/julianw1010/numactl-WASP):

```bash
numactl-wasp -r all ./my_application
```

### Per-process control via prctl

```c
#include <sys/prctl.h>

// Enable on all online nodes
prctl(PR_SET_PGTABLE_REPL, 1, 0, 0, 0);

// Enable on specific nodes (bitmask: nodes 0 and 1)
prctl(PR_SET_PGTABLE_REPL, 0x3, 0, 0, 0);

// Enable for another process by PID
prctl(PR_SET_PGTABLE_REPL, 1, target_pid, 0, 0);

// Disable
prctl(PR_SET_PGTABLE_REPL, 0, 0, 0, 0);

// Cache-only mode: NUMA-aware page table allocation without full replication
prctl(PR_SET_PGTABLE_CACHE_ONLY, 1, 0, 0, 0);

// Steering: control which replica a node uses
int steering[NUMA_NODE_COUNT] = { 0, 1, 0, 1, -1, -1, -1, -1 };
prctl(PR_SET_PGTABLE_REPL_STEERING, steering, 0, 0, 0);
```

## Runtime Modes

**Full replication** creates and maintains a complete page table replica on each configured NUMA node. Writes propagate to all replicas. CR3 is loaded from the local (or steered) replica on context switch.

**Cache-only mode** uses the per-node page table cache for NUMA-aware allocation without creating replicas. Page table pages are allocated from the node where the fault occurs, but no broadcast or CR3 switching takes place.

**Steering** overrides which replica a given CPU node reads from. By default each node uses its own replica. The steering table can redirect a node to use another node's replica, for example when NUMA congestion makes a remote node's memory faster to access than local memory. WASP computes the steering table automatically from measured latencies.

## Inheritance and execve

Child processes inherit replication state from their parent during `fork`, controlled by `sysctl_mitosis_inherit` (default: enabled). Replication state also survives `execve`: the new mm receives the parent's node set and enables replication after the new binary is loaded.

When `sysctl_mitosis_mode` is set to 1 (or the `mitosis` boot parameter is used), new processes that do not inherit replication from a parent receive replication across all online nodes automatically.

## Page Table Isolation (PTI)

Mitosis supports kernels with KPTI enabled. Replica PGDs are allocated at the correct order (two pages for kernel and user halves), and user-half PGD entries are propagated alongside kernel-half entries. PCID and INVPCID are disabled at boot to simplify CR3 switching between replicas.

## Transparent Huge Pages

Mitosis works with THP under any defrag policy. THP split operations free stale PTE replicas before repopulating, and huge PMD entries are broadcast to all replicas. Accessed and dirty bits are aggregated across replicas for correct huge page fault handling.

## Per-Node Page Table Cache

Mitosis maintains a per-node free list of zeroed page table pages. Freed page table pages are returned to their node's cache instead of the buddy allocator when possible, and new allocations check the cache first. The cache reduces allocation latency and ensures node-local placement.

Cache statistics and control are available through `/proc/mitosis/cache`. Writing `-1` drains all caches; writing a positive number `N` pre-populates `N` pages per node.

## procfs Interface

All kernel-side runtime state is exposed under `/proc/mitosis/`:

**cache** shows per-node cache statistics (count, hits, misses, returns). Writable for cache management.

**mode** shows and controls the global replication mode (-1: default allocation, 0: force node 0, 1: auto-enable for new processes).

**inherit** shows and controls whether child processes inherit replication state (1: enabled, -1: disabled).

**verify** enables or disables fault-time consistency verification. When enabled, every fault checks that all page table pages walked from the active CR3 reside on the expected node, and panics on mismatch.

## License

GPL-2.0 (same as the Linux kernel).

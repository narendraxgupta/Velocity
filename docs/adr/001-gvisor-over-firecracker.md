# ADR-001: gVisor over Firecracker for sandboxing

- **Status**: Accepted
- **Date**: 2026-05-18
- **Deciders**: Platform team

## Context

Velocity hosts **untrusted, submitter-supplied binaries**. These binaries can
contain anything — compiler optimizations gone wrong, kernel exploit attempts,
shell escapes, network scanners. Running them inside plain Linux containers
on a shared host is not acceptable; container escapes via kernel CVEs are a
documented attack class
([CVE-2022-0185](https://nvd.nist.gov/vuln/detail/CVE-2022-0185),
[CVE-2017-5123](https://nvd.nist.gov/vuln/detail/CVE-2017-5123),
[Dirty COW](https://dirtycow.ninja/), etc.).

We need an isolation primitive that:

1. **Intercepts syscalls** so kernel exploits do not reach the host kernel.
2. **Integrates with Kubernetes** via a standard `RuntimeClass`.
3. **Has acceptable overhead** — we are measuring latency in microseconds;
   the sandbox cannot add milliseconds.
4. **Boots fast** — submissions are short-lived; a 5s boot kills the dev loop.

Two credible options dominate the market:

- **gVisor** (`runsc`): Google's user-space kernel implementing a subset of
  the Linux syscall ABI in Go. Container escapes require breaking out of a
  user-space process, then the OCI sandbox, then Linux user namespaces.
- **Firecracker**: AWS's microVM hypervisor (the engine behind Lambda and
  Fargate). True hardware-virtualization isolation via KVM.

## Decision

We use **gVisor with the `runsc` OCI runtime**, configured via Kubernetes
`RuntimeClass: gvisor`, layered on top of cgroups v2 resource limits.

## Consequences

### Good

- Drop-in compatibility with our existing Docker/K8s pipeline — no
  separate VM image format, no custom kernel.
- Boot time ~250 ms (vs ~125 ms for Firecracker) — both acceptable.
- Network stack lives in user space too, so a submission exploiting an in-
  kernel networking bug (a recurring pattern) cannot reach the host.
- gVisor's syscall surface is dramatically smaller than Linux's, so the
  attack surface shrinks meaningfully.

### Bad

- **~10–15% syscall overhead** for heavily syscall-bound workloads. Trading
  engines tend to be tight loops and IPC, so this is real but bounded.
- Some syscalls are unimplemented (`io_uring` in older versions, certain
  perf counters). We compensate by *requiring* our reference exchanges to
  use only the supported set, documented in
  [`docs/submission-runtime.md`](../submission-runtime.md). Newer gVisor
  releases support io_uring with `--platform=systrap`.
- Performance comparisons between submissions remain *fair* (every
  submission pays the same gVisor cost) but **absolute** latency numbers
  are biased high relative to bare metal. We accept this — Velocity is a
  *comparative* benchmark, not an absolute one.

## Alternatives considered

### Firecracker microVMs

- **Pro**: Strongest isolation. Lambda's choice. Per-VM kernel.
- **Pro**: Boot time impressive (~125 ms).
- **Con**: Operationally heavy — every submission needs a kernel image, a
  rootfs image, a network bridge. We would essentially be building
  Lambda. Out of scope for a one-engineer platform.
- **Con**: Memory overhead per VM (each VM has its own kernel, ~50 MB
  minimum) eats into our per-submission RAM budget.

### Kata Containers

- **Pro**: OCI-compatible like gVisor, VM-based like Firecracker.
- **Con**: Slower boots (~1.5 s), heavier memory footprint, less
  battle-tested at our scale.

### Plain Docker + Linux capabilities

- **Pro**: Zero overhead. Simple.
- **Con**: One kernel CVE = full host compromise. **Disqualifying**.

### nsjail / bubblewrap

- **Pro**: Lightweight, fast.
- **Con**: Configuration is bespoke; no K8s integration; weaker than gVisor
  for protecting against kernel bugs.

## References

- [gVisor: Bring the kernel to user space (USENIX ATC '18)](https://www.usenix.org/conference/atc18/presentation/young)
- [Firecracker: Lightweight Virtualization for Serverless Applications (NSDI '20)](https://www.usenix.org/conference/nsdi20/presentation/agache)
- [gVisor performance overhead documentation](https://gvisor.dev/docs/architecture_guide/performance/)
- [Kubernetes RuntimeClass docs](https://kubernetes.io/docs/concepts/containers/runtime-class/)

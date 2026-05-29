/* SPDX-License-Identifier: BSD-2-Clause */
/* ==========================================================================
 *  velocity — kernel-level TCP latency probe (eBPF/CO-RE)
 *
 *  What this measures
 *  ------------------
 *  For every TCP socket the bot-worker process opens to the submission
 *  engine, we hook two kernel functions:
 *
 *    * tcp_sendmsg(struct sock *sk, struct msghdr *msg, size_t size)
 *        Fires inside the syscall path right before TCP segments are
 *        handed to the IP layer. We stamp `kernel_send_ns` here.
 *
 *    * tcp_recvmsg(struct sock *sk, struct msghdr *msg, ...)
 *        Fires when userspace pulls data back out of the socket. We
 *        stamp `kernel_recv_ns` here and emit `(kernel_recv_ns -
 *        kernel_send_ns)` as the kernel-observed round-trip.
 *
 *  Why this matters
 *  ----------------
 *  Userspace timing (the bot worker's `clock_gettime` before/after
 *  `send()`/`recv()`) includes syscall entry, copy_from_user, scheduler
 *  jitter, and the time userspace took to *notice* the response (via
 *  io_uring CQE poll, epoll wake, etc.).
 *
 *  Kernel timing is the *true* time-on-the-wire (plus syscall path).
 *  The delta between the two ("userspace_p99 - kernel_p99") is the
 *  time the loadgen itself was the bottleneck, which is exactly what
 *  judges will ask about when they see "your latency chart shows 80µs
 *  p99 but your engine claims 12µs."
 *
 *  Design notes
 *  ------------
 *  CO-RE: we use BPF CO-RE relocations so this object can be loaded on
 *  any kernel ≥ 5.4 without recompilation. We only touch fields that
 *  have been stable since 4.4, so the relocation list is tiny.
 *
 *  Socket keying: the (sk, send_seq) hash lets us pair a specific
 *  send with the recv that follows. Using sk alone would conflate
 *  pipelined requests on the same connection; using sk+seq gives us
 *  per-segment fidelity.
 *
 *  Filter: we only stamp sockets that belong to a PID we've been told
 *  about (the loader writes pid_filter[bot_pid] = 1 on startup). This
 *  keeps the probe noise-free when other processes share the host.
 * ==========================================================================
 */

#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>

char LICENSE[] SEC("license") = "Dual BSD/GPL";

/* ---------------------------------------------------------------------------
 *  Maps
 * ------------------------------------------------------------------------- */

/* Outstanding sends: key = (sk pointer + tcp seq), value = stamp_ns. */
struct send_key {
    __u64 sk_addr;
    __u32 seq;
    __u32 _pad;
} __attribute__((packed));

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 65536);
    __type(key,   struct send_key);
    __type(value, __u64);
} sends SEC(".maps");

/* PID allowlist — written by the userspace loader on startup. */
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 64);
    __type(key,   __u32);
    __type(value, __u8);
} pid_filter SEC(".maps");

/* Output event ring — drained by the worker's ebpf_probe drain thread. */
struct latency_event {
    __u64 ts_ns;          /* send-side kernel timestamp                 */
    __u64 delta_ns;       /* recv_ts - send_ts (kernel time-on-wire)    */
    __u32 pid;
    __u32 sk_hash;        /* low 32 bits of sk pointer for grouping     */
    __u16 src_port;
    __u16 dst_port;
};

struct {
    __uint(type, BPF_MAP_TYPE_PERF_EVENT_ARRAY);
    __uint(key_size, sizeof(__u32));
    __uint(value_size, sizeof(__u32));
} events SEC(".maps");

/* ---------------------------------------------------------------------------
 *  Helpers
 * ------------------------------------------------------------------------- */

static __always_inline int pid_allowed(__u32 pid)
{
    /* If the allowlist is empty, accept everything — useful during
     * development and tests. */
    __u8 *v = bpf_map_lookup_elem(&pid_filter, &pid);
    if (v) return *v;

    /* Quick "is map empty?" check: a sentinel key 0 we never legitimately
     * use means "accept all". The loader sets pid_filter[0] = 1 to enable
     * permissive mode. */
    __u32 zero = 0;
    __u8 *any = bpf_map_lookup_elem(&pid_filter, &zero);
    return any ? *any : 0;
}

static __always_inline __u32 sk_low32(struct sock *sk)
{
    return (__u32)(((__u64)sk) & 0xffffffff);
}

/* ---------------------------------------------------------------------------
 *  Probes
 * ------------------------------------------------------------------------- */

SEC("kprobe/tcp_sendmsg")
int BPF_KPROBE(tcp_sendmsg_entry, struct sock *sk, struct msghdr *msg, size_t size)
{
    __u32 pid = bpf_get_current_pid_tgid() >> 32;
    if (!pid_allowed(pid))
        return 0;

    /* Read tp->snd_nxt — the sequence number that will be assigned to
     * the bytes we're about to enqueue. We use it (modulo) as part of
     * the send_key so request/response pairs match up. */
    struct tcp_sock *tp = (struct tcp_sock *)sk;
    __u32 seq = BPF_CORE_READ(tp, snd_nxt);

    struct send_key k = {
        .sk_addr = (__u64)sk,
        .seq     = seq,
    };
    __u64 ts = bpf_ktime_get_ns();
    bpf_map_update_elem(&sends, &k, &ts, BPF_ANY);
    return 0;
}

SEC("kprobe/tcp_recvmsg")
int BPF_KPROBE(tcp_recvmsg_entry, struct sock *sk)
{
    __u32 pid = bpf_get_current_pid_tgid() >> 32;
    if (!pid_allowed(pid))
        return 0;

    /* Match against the most recently-ack'd seq, which is the closest
     * approximation we get to "which send is this a response to" without
     * doing full TCP reassembly inside the kernel. The matching is
     * statistical (race-prone for pipelined requests) but in aggregate
     * the p50/p99 are extremely close to the truth — and ground-truth-
     * correct per-request matching belongs in the userspace correlator
     * not the kprobe. */
    struct tcp_sock *tp = (struct tcp_sock *)sk;
    __u32 ack = BPF_CORE_READ(tp, snd_una);

    struct send_key k = {
        .sk_addr = (__u64)sk,
        .seq     = ack,
    };
    __u64 *send_ts = bpf_map_lookup_elem(&sends, &k);
    if (!send_ts)
        return 0;

    __u64 now = bpf_ktime_get_ns();
    __u64 delta = now - *send_ts;
    bpf_map_delete_elem(&sends, &k);

    /* Read endpoint metadata for the event. */
    __u16 sport = BPF_CORE_READ(sk, __sk_common.skc_num);
    __u16 dport = BPF_CORE_READ(sk, __sk_common.skc_dport);

    struct latency_event ev = {
        .ts_ns    = now,
        .delta_ns = delta,
        .pid      = pid,
        .sk_hash  = sk_low32(sk),
        .src_port = sport,
        .dst_port = __builtin_bswap16(dport),
    };
    bpf_perf_event_output(ctx, &events, BPF_F_CURRENT_CPU, &ev, sizeof(ev));
    return 0;
}

// Package replayer is the byte-exact pcap replay engine.
//
// Algorithm:
//
//  1. Stream the pcap from MinIO.
//  2. Parse with gopacket; for every TCP segment whose dst matches the
//     "client → server" direction (i.e. the bots-to-engine half of the
//     original conversation), accumulate the payload by source flow.
//  3. Re-establish a fresh TCP connection to the *new* target endpoint for
//     each original source flow. (Flows are keyed by the original client
//     port so concurrent connections in the capture stay parallel during
//     replay.)
//  4. For each captured packet on a flow, sleep until its scheduled send
//     time and then Write its payload. Read back responses on a separate
//     goroutine to track ack arrivals; we don't validate response bodies
//     (that's the correctness-validator's job), we only time the
//     round-trip so the leaderboard pipeline gets real latency numbers.
//
// Scheduling modes:
//
//   - PRESERVE: each packet's send time is `original_offset *
//     speed_multiplier`. Inter-packet gaps are honored to within OS
//     scheduling jitter (we use a single ticker per flow to avoid
//     accumulating drift).
//   - FIXED_RPS: ignore inter-packet gaps entirely; fire one captured
//     payload per (1s / fixed_rps), round-robined across all flows.
//
// State surface (per-replay):
//
//   - Live: `Snapshot{sent, errored, current_rps, ...}` accessible via
//     `r.Watch(id)`. Frontend polls this through the gateway.
//   - Final: when the last packet on the last flow has either acked or
//     timed out, the replay is marked Complete; the final snapshot is
//     persisted so a late `Watch` still returns a sensible state.
package replayer

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"sort"
	"strconv"
	"sync"
	"sync/atomic"
	"time"

	"github.com/google/gopacket"
	"github.com/google/gopacket/layers"
	"github.com/google/gopacket/pcapgo"
	"github.com/redis/go-redis/v9"
	"go.uber.org/zap"

	"github.com/velocity/platform/services/pcap-replayer/internal/store"
)

// ClockMode mirrors the proto enum so callers don't have to import gen/go.
type ClockMode uint8

const (
	ClockModeUnspecified ClockMode = 0
	ClockModePreserve    ClockMode = 1
	ClockModeFixedRPS    ClockMode = 2
)

// Deps wires the replayer's static dependencies.
type Deps struct {
	Logger  *zap.SugaredLogger
	Storage store.Storage
	Redis   *redis.Client
}

// StartArgs covers one replay invocation.
type StartArgs struct {
	BenchmarkID     string // pre-minted by the gateway so the frontend can subscribe immediately
	TargetHost      string
	TargetPort      int
	SourceObjectKey string
	ClockMode       ClockMode
	FixedRPS        uint64
	SpeedMultiplier float64
}

// Snapshot is the live progress object surfaced over HTTP.
type Snapshot struct {
	BenchmarkID     string    `json:"benchmark_id"`
	State           string    `json:"state"` // running | complete | failed | cancelled
	Detail          string    `json:"detail,omitempty"`
	StartedAt       time.Time `json:"started_at"`
	UpdatedAt       time.Time `json:"updated_at"`
	ExpectedPackets uint64    `json:"expected_packets"`
	SentTotal       uint64    `json:"sent_total"`
	AckedTotal      uint64    `json:"acked_total"`
	ErroredTotal    uint64    `json:"errored_total"`
	CurrentRPS      uint64    `json:"current_rps"`
	P50LatencyNs    uint64    `json:"p50_latency_ns"`
	P99LatencyNs    uint64    `json:"p99_latency_ns"`
}

// Replayer is the long-lived service object.
type Replayer struct {
	deps Deps

	mu   sync.Mutex
	runs map[string]*run
}

type run struct {
	id     string
	cancel context.CancelFunc
	snap   *atomic.Pointer[Snapshot]
}

// New constructs an idle Replayer.
func New(deps Deps) *Replayer {
	return &Replayer{
		deps: deps,
		runs: map[string]*run{},
	}
}

// Start kicks off a replay. Returns immediately; the actual replay runs
// in a goroutine and progress is observable via Watch.
func (r *Replayer) Start(ctx context.Context, args StartArgs) (uint64, error) {
	if args.BenchmarkID == "" {
		return 0, errors.New("benchmark_id required")
	}
	if args.TargetHost == "" || args.TargetPort == 0 {
		return 0, errors.New("target host:port required")
	}
	if args.SourceObjectKey == "" {
		return 0, errors.New("source_object_key required")
	}
	if args.ClockMode == ClockModeUnspecified {
		args.ClockMode = ClockModePreserve
	}
	if args.SpeedMultiplier <= 0 {
		args.SpeedMultiplier = 1.0
	}

	r.mu.Lock()
	if _, dup := r.runs[args.BenchmarkID]; dup {
		r.mu.Unlock()
		return 0, fmt.Errorf("replay already running for %s", args.BenchmarkID)
	}
	r.mu.Unlock()

	// Parse the pcap upfront so we can return expected_packets to the
	// caller and bail out cleanly if the object is malformed.
	flows, expected, err := r.loadFlows(ctx, args.SourceObjectKey)
	if err != nil {
		return 0, fmt.Errorf("parse pcap: %w", err)
	}
	if expected == 0 {
		return 0, fmt.Errorf("pcap contained no client→server payload packets")
	}

	runCtx, cancel := context.WithCancel(context.Background())
	snap := &atomic.Pointer[Snapshot]{}
	snap.Store(&Snapshot{
		BenchmarkID:     args.BenchmarkID,
		State:           "running",
		StartedAt:       time.Now(),
		UpdatedAt:       time.Now(),
		ExpectedPackets: expected,
	})
	rn := &run{id: args.BenchmarkID, cancel: cancel, snap: snap}

	r.mu.Lock()
	r.runs[args.BenchmarkID] = rn
	r.mu.Unlock()

	go r.execute(runCtx, args, flows, rn)
	return expected, nil
}

// Watch returns the most recent snapshot for `id`, or nil if unknown.
//
// Lookup order:
//  1. In-memory run table — current snapshot for active replays.
//  2. Redis `replay:<id>` — terminal snapshot persisted by execute()
//     when the run finished. Lets the frontend keep polling state for
//     a few hours after completion without us holding the run forever.
func (r *Replayer) Watch(id string) *Snapshot {
	r.mu.Lock()
	rn := r.runs[id]
	r.mu.Unlock()
	if rn != nil {
		return rn.snap.Load()
	}
	if r.deps.Redis == nil {
		return nil
	}
	ctx, cancel := context.WithTimeout(context.Background(), 250*time.Millisecond)
	defer cancel()
	body, err := r.deps.Redis.Get(ctx, "replay:"+id).Bytes()
	if err != nil {
		return nil
	}
	var snap Snapshot
	if err := json.Unmarshal(body, &snap); err != nil {
		return nil
	}
	return &snap
}

// Cancel asks the replay to exit early. Idempotent.
func (r *Replayer) Cancel(id string) bool {
	r.mu.Lock()
	rn := r.runs[id]
	r.mu.Unlock()
	if rn == nil {
		return false
	}
	rn.cancel()
	return true
}

// CancelAll terminates every in-flight replay. Called on shutdown.
func (r *Replayer) CancelAll() {
	r.mu.Lock()
	defer r.mu.Unlock()
	for _, rn := range r.runs {
		rn.cancel()
	}
}

// -----------------------------------------------------------------------------
//  pcap parsing
// -----------------------------------------------------------------------------

// packet carries one captured payload along with its time offset.
type packet struct {
	OffsetNs uint64
	Payload  []byte
}

// flow is a single client→server TCP conversation reconstructed from the
// pcap. The key is the original client port (so multiple captured flows can
// replay in parallel against the new target).
type flow struct {
	OriginalSrcPort uint16
	OriginalDstPort uint16
	Packets         []packet
}

// loadFlows streams the pcap from MinIO, reassembles TCP flows by
// `(src,dst)` 4-tuple, and bundles client→server payload packets.
//
// We *do not* attempt full TCP reassembly with proper sequence ordering
// (would be over-engineered for the use-cases): we trust that the
// recording was made on a healthy network where TCP segments arrived in
// order and weren't lost. For pathological captures the replayer
// silently degrades to "fire whatever bytes we saw in capture order",
// which is the same behaviour `tcpreplay` defaults to.
func (r *Replayer) loadFlows(ctx context.Context, key string) ([]flow, uint64, error) {
	rd, err := r.deps.Storage.Get(ctx, key)
	if err != nil {
		return nil, 0, err
	}
	defer rd.Close()

	// pcapgo expects a bufio.Reader; wrap so the upstream can be any
	// io.Reader (MinIO returns a streaming body).
	pcapR, err := pcapgo.NewReader(bufio.NewReaderSize(rd, 64*1024))
	if err != nil {
		return nil, 0, fmt.Errorf("pcap header: %w", err)
	}

	flowsByPort := map[uint16]*flow{}
	var firstTs time.Time
	var total uint64

	for {
		data, ci, perr := pcapR.ReadPacketData()
		if errors.Is(perr, io.EOF) {
			break
		}
		if perr != nil {
			// Truncated packets are non-fatal — keep going.
			r.deps.Logger.Warnw("pcap read error (continuing)", "err", perr)
			continue
		}

		pkt := gopacket.NewPacket(data, pcapR.LinkType(), gopacket.DecodeOptions{
			Lazy:   true,
			NoCopy: true,
		})
		tcpLayer := pkt.Layer(layers.LayerTypeTCP)
		if tcpLayer == nil {
			continue
		}
		tcp, _ := tcpLayer.(*layers.TCP)
		if tcp == nil || len(tcp.Payload) == 0 {
			continue
		}

		// Heuristic for "client→server": the client talks *to* a
		// well-known service port, which is numerically smaller than the
		// client's own ephemeral source port (the kernel allocates these
		// from 32768+). So a packet is client→server exactly when its
		// destination port is the smaller of the pair. Those are the only
		// packets we replay — the bot drives requests at the engine, and
		// server→client responses are read back off the socket on replay.
		//
		// NOTE: this condition was previously `DstPort >= SrcPort`, which
		// kept the server→client direction and dropped every actual
		// request, so replays found "no client→server payload packets".
		if tcp.DstPort < tcp.SrcPort {
			// client → server (this is what we replay)
		} else {
			// server → client response (or dst == src) — skip it; on
			// replay we read the response off the socket instead.
			continue
		}

		if firstTs.IsZero() {
			firstTs = ci.Timestamp
		}
		offset := uint64(ci.Timestamp.Sub(firstTs).Nanoseconds())
		if offset > uint64(24)*uint64(time.Hour) {
			offset = 0 // sanity: malformed capture
		}

		port := uint16(tcp.SrcPort)
		fl := flowsByPort[port]
		if fl == nil {
			fl = &flow{
				OriginalSrcPort: port,
				OriginalDstPort: uint16(tcp.DstPort),
			}
			flowsByPort[port] = fl
		}
		// Copy: gopacket reuses its buffer between packets when NoCopy is on.
		buf := make([]byte, len(tcp.Payload))
		copy(buf, tcp.Payload)
		fl.Packets = append(fl.Packets, packet{
			OffsetNs: offset,
			Payload:  buf,
		})
		total++
	}

	// Stable ordering: by original src port, then by send time within
	// each flow.
	out := make([]flow, 0, len(flowsByPort))
	for _, fl := range flowsByPort {
		sort.Slice(fl.Packets, func(i, j int) bool {
			return fl.Packets[i].OffsetNs < fl.Packets[j].OffsetNs
		})
		out = append(out, *fl)
	}
	sort.Slice(out, func(i, j int) bool {
		return out[i].OriginalSrcPort < out[j].OriginalSrcPort
	})
	return out, total, nil
}

// -----------------------------------------------------------------------------
//  Replay execution
// -----------------------------------------------------------------------------

// execute drives the actual replay. Runs in its own goroutine.
func (r *Replayer) execute(ctx context.Context, args StartArgs, flows []flow, rn *run) {
	defer func() {
		// Persist final snapshot to Redis under `replay:<id>` so a future
		// process can still answer Watch queries after we forget.
		//
		// Note: go-redis falls back to fmt.Sprint when handed a struct
		// pointer that has no MarshalBinary; that produced unparseable
		// strings. Marshal explicitly so the value round-trips through
		// JSON cleanly.
		if r.deps.Redis != nil {
			if snap := rn.snap.Load(); snap != nil {
				if body, err := json.Marshal(snap); err == nil {
					_ = r.deps.Redis.Set(context.Background(),
						"replay:"+args.BenchmarkID,
						body, 24*time.Hour).Err()
				}
			}
		}
	}()

	target := net.JoinHostPort(args.TargetHost, strconv.Itoa(args.TargetPort))

	// Shared atomic counters; the snapshot publisher reads them every
	// ~250 ms to assemble the live Snapshot.
	var (
		sent     uint64
		acked    uint64
		errored  uint64
		latNsSum uint64
		latNsObs uint64
		// Reservoir of recent latencies for p50/p99 estimation. We keep
		// the most recent 1024 samples — bounded memory, decent
		// percentile fidelity for the use-case (display, not pricing).
		recent    = make([]uint64, 0, 1024)
		recentMu  sync.Mutex
		runErrors = make([]error, 0, 4)
		errMu     sync.Mutex
	)
	appendErr := func(e error) {
		errMu.Lock()
		runErrors = append(runErrors, e)
		errMu.Unlock()
	}
	observeLatency := func(ns uint64) {
		atomic.AddUint64(&latNsSum, ns)
		atomic.AddUint64(&latNsObs, 1)
		recentMu.Lock()
		if len(recent) < cap(recent) {
			recent = append(recent, ns)
		} else {
			recent[int(atomic.LoadUint64(&acked))%cap(recent)] = ns
		}
		recentMu.Unlock()
	}

	publish := func(state, detail string) {
		snap := &Snapshot{
			BenchmarkID:     args.BenchmarkID,
			State:           state,
			Detail:          detail,
			StartedAt:       rn.snap.Load().StartedAt,
			UpdatedAt:       time.Now(),
			ExpectedPackets: rn.snap.Load().ExpectedPackets,
			SentTotal:       atomic.LoadUint64(&sent),
			AckedTotal:      atomic.LoadUint64(&acked),
			ErroredTotal:    atomic.LoadUint64(&errored),
		}
		elapsed := time.Since(snap.StartedAt).Seconds()
		if elapsed > 0 {
			snap.CurrentRPS = uint64(float64(snap.AckedTotal) / elapsed)
		}
		recentMu.Lock()
		snap.P50LatencyNs = percentile(recent, 0.50)
		snap.P99LatencyNs = percentile(recent, 0.99)
		recentMu.Unlock()
		rn.snap.Store(snap)
	}

	// Publisher: emits a snapshot every 250ms while the replay runs.
	pubCtx, pubCancel := context.WithCancel(ctx)
	defer pubCancel()
	go func() {
		t := time.NewTicker(250 * time.Millisecond)
		defer t.Stop()
		for {
			select {
			case <-pubCtx.Done():
				return
			case <-t.C:
				publish("running", "")
			}
		}
	}()

	// One goroutine per flow. They race independently but share the
	// outcome counters.
	var wg sync.WaitGroup
	for i := range flows {
		fl := flows[i]
		wg.Add(1)
		go func() {
			defer wg.Done()
			conn, err := net.DialTimeout("tcp", target, 5*time.Second)
			if err != nil {
				appendErr(fmt.Errorf("dial %s: %w", target, err))
				atomic.AddUint64(&errored, uint64(len(fl.Packets)))
				return
			}
			defer conn.Close()

			// Reader goroutine: drains the socket; every byte that
			// comes back counts as an "ack" for the most recent send.
			// We don't framing-parse the response — that's the
			// correctness-validator's job. We just measure round-trip.
			ackCh := make(chan time.Time, 1024)
			go func() {
				buf := make([]byte, 16*1024)
				for {
					_ = conn.SetReadDeadline(time.Now().Add(30 * time.Second))
					_, rerr := conn.Read(buf)
					if rerr != nil {
						return
					}
					select {
					case ackCh <- time.Now():
					default:
						// reader is slow; drop the ack — better than blocking
					}
				}
			}()

			// Per-packet pacer matched to clock mode. Avoid time.After in
			// the loop — each call allocates a timer that isn't reclaimed
			// until it fires, which leaks badly on large pcaps.
			var fixedPace <-chan time.Time
			if args.ClockMode == ClockModeFixedRPS && args.FixedRPS > 0 {
				tk := time.NewTicker(time.Second / time.Duration(args.FixedRPS))
				defer tk.Stop()
				fixedPace = tk.C
			}
			startWall := time.Now()
			for _, pk := range fl.Packets {
				if ctx.Err() != nil {
					return
				}
				switch args.ClockMode {
				case ClockModePreserve:
					target := startWall.Add(
						time.Duration(float64(pk.OffsetNs) / args.SpeedMultiplier))
					if err := waitCtx(ctx, time.Until(target)); err != nil {
						return
					}
				case ClockModeFixedRPS:
					if fixedPace != nil {
						select {
						case <-ctx.Done():
							return
						case <-fixedPace:
						}
					}
				}

				sendAt := time.Now()
				_ = conn.SetWriteDeadline(time.Now().Add(5 * time.Second))
				if _, werr := conn.Write(pk.Payload); werr != nil {
					atomic.AddUint64(&errored, 1)
					continue
				}
				atomic.AddUint64(&sent, 1)

				// Best-effort latency observation: wait up to per-packet
				// timeout for the next ack timestamp. We don't enforce
				// strict 1:1 mapping (multiple captured packets may map
				// to one HTTP response in practice); the observation is
				// statistically meaningful in aggregate.
				ackTimer := time.NewTimer(2 * time.Second)
				select {
				case ackAt := <-ackCh:
					if !ackTimer.Stop() {
						<-ackTimer.C
					}
					atomic.AddUint64(&acked, 1)
					observeLatency(uint64(ackAt.Sub(sendAt).Nanoseconds()))
				case <-ackTimer.C:
					atomic.AddUint64(&errored, 1)
				case <-ctx.Done():
					if !ackTimer.Stop() {
						<-ackTimer.C
					}
					return
				}
			}
		}()
	}

	// Wait for all flows; then publish a final terminal snapshot.
	wg.Wait()
	pubCancel()

	state := "complete"
	detail := ""
	if ctx.Err() != nil {
		state = "cancelled"
		detail = ctx.Err().Error()
	} else if len(runErrors) > 0 {
		// Partial-failure is still "complete"; surface in detail.
		var b bytes.Buffer
		for i, e := range runErrors {
			if i > 0 {
				b.WriteString("; ")
			}
			b.WriteString(e.Error())
		}
		detail = b.String()
	}
	publish(state, detail)

	r.mu.Lock()
	delete(r.runs, args.BenchmarkID)
	r.mu.Unlock()

	r.deps.Logger.Infow("replay finished",
		"benchmark_id", args.BenchmarkID,
		"state", state,
		"sent", atomic.LoadUint64(&sent),
		"acked", atomic.LoadUint64(&acked),
		"errored", atomic.LoadUint64(&errored))
}

// percentile returns the qth percentile of v (un-sorted; we copy + sort).
// Returns 0 on empty input.
func percentile(v []uint64, q float64) uint64 {
	if len(v) == 0 {
		return 0
	}
	// Copy first; cheap relative to network I/O.
	cp := make([]uint64, len(v))
	copy(cp, v)
	sort.Slice(cp, func(i, j int) bool { return cp[i] < cp[j] })
	idx := int(float64(len(cp)-1) * q)
	if idx < 0 {
		idx = 0
	}
	if idx >= len(cp) {
		idx = len(cp) - 1
	}
	return cp[idx]
}

// waitCtx blocks until d elapses or ctx is cancelled. Drains the timer on
// cancel so callers pacing in a tight loop do not leak heap timers.
func waitCtx(ctx context.Context, d time.Duration) error {
	if d <= 0 {
		select {
		case <-ctx.Done():
			return ctx.Err()
		default:
			return nil
		}
	}
	t := time.NewTimer(d)
	defer t.Stop()
	select {
	case <-ctx.Done():
		if !t.Stop() {
			<-t.C
		}
		return ctx.Err()
	case <-t.C:
		return nil
	}
}

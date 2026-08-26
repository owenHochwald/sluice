// Package workload is the unit of work surge performs against a target: one
// fresh TCP connection, mirroring how sluiced picks a backend per-connection
// rather than per-request.
package workload

import (
	"bufio"
	"bytes"
	"context"
	"fmt"
	"io"
	"net"
	"time"
)

// Config parameterizes one workload attempt.
type Config struct {
	Target      string
	PayloadSize int
	DialTimeout time.Duration
	IOTimeout   time.Duration
}

// Outcome is the result of one workload attempt.
type Outcome struct {
	// Intended is the scheduled time this attempt was meant to start at.
	Intended time.Time
	// Latency is measured from Intended, not from actual dispatch, so
	// scheduler-side dispatch delay under load is folded into what gets
	// recorded (docs/SPEC.md LG-U-03 — this is what makes the histogram
	// coordinated-omission-correct).
	Latency time.Duration
	// InstanceID is the backend identifier echod emitted on connect, empty
	// if none was received (e.g. the connection was fault-aborted before
	// the ID line, or Err is set).
	InstanceID string
	// PayloadMismatch is true if the echoed bytes didn't match what was
	// sent. Recorded, not treated as a hard error — byte-identity is
	// TST-03's job, not this load generator's.
	PayloadMismatch bool
	// Err is set if dial, read, or write failed at any step. An Outcome
	// with Err set is a failure and must not be folded into a success
	// latency histogram.
	Err error
}

// Do performs one workload attempt: dial, read the "ID <id>\n" line, write
// a payload, read it back, compare. ctx bounds only the dial; once
// established, the connection is bounded by cfg.IOTimeout instead, since a
// caller cancelling ctx mid-run (e.g. on SIGTERM) shouldn't abort a
// connection that's already in flight.
func Do(ctx context.Context, cfg Config, intended time.Time, payload []byte) Outcome {
	out := Outcome{Intended: intended}

	dialer := net.Dialer{Timeout: cfg.DialTimeout}
	conn, err := dialer.DialContext(ctx, "tcp", cfg.Target)
	if err != nil {
		out.Err = fmt.Errorf("dial: %w", err)
		out.Latency = time.Since(intended)
		return out
	}
	defer conn.Close()

	// A single deadline for the whole exchange: this is what bounds
	// echod's Hang fault to a finite failure instead of leaking the
	// goroutine that dispatched this attempt.
	if cfg.IOTimeout > 0 {
		_ = conn.SetDeadline(time.Now().Add(cfg.IOTimeout))
	}

	r := bufio.NewReader(conn)
	line, err := r.ReadString('\n')
	if err != nil {
		out.Err = fmt.Errorf("read id line: %w", err)
		out.Latency = time.Since(intended)
		return out
	}
	if id, ok := parseIDLine(line); ok {
		out.InstanceID = id
	}

	if _, err := conn.Write(payload); err != nil {
		out.Err = fmt.Errorf("write payload: %w", err)
		out.Latency = time.Since(intended)
		return out
	}

	echoed := make([]byte, len(payload))
	if _, err := io.ReadFull(r, echoed); err != nil {
		out.Err = fmt.Errorf("read echo: %w", err)
		out.Latency = time.Since(intended)
		return out
	}

	out.Latency = time.Since(intended)
	out.PayloadMismatch = !bytes.Equal(payload, echoed)
	return out
}

// parseIDLine parses echod's "ID <id>\n" greeting.
func parseIDLine(line string) (id string, ok bool) {
	const prefix = "ID "
	line = trimTrailingNewline(line)
	if len(line) <= len(prefix) || line[:len(prefix)] != prefix {
		return "", false
	}
	return line[len(prefix):], true
}

func trimTrailingNewline(s string) string {
	for len(s) > 0 && (s[len(s)-1] == '\n' || s[len(s)-1] == '\r') {
		s = s[:len(s)-1]
	}
	return s
}

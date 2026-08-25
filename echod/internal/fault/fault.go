// Package fault holds echod's fault-injection configuration behind an
// atomic pointer so it can be swapped by the admin API mid-flight, without
// restarting the listener or disturbing connections already in progress
// (docs/SPEC.md BE-E-01).
package fault

import (
	"sync/atomic"
	"time"
)

// Config controls how a connection is treated. The zero value is
// well-behaved: pure echo, no faults.
type Config struct {
	// LatencyMin/LatencyMax bound a uniform random delay applied before each
	// echoed write (BE-O-01). Zero values mean no delay.
	LatencyMin time.Duration
	LatencyMax time.Duration

	// ErrorRate is the fraction of connections, in [0,1], aborted
	// immediately after accept instead of served (BE-O-02).
	ErrorRate float64

	// Hang, when true, makes echod accept the connection and never write
	// anything to it (BE-O-03).
	Hang bool

	// SlowStartFor adds LatencyMin/LatencyMax-independent extra delay for
	// this long after process start (BE-O-04). Zero means no slow start.
	SlowStartFor   time.Duration
	SlowStartExtra time.Duration
}

// Store is a concurrency-safe holder for the live Config.
type Store struct {
	ptr   atomic.Pointer[Config]
	start time.Time
}

func NewStore(initial Config) *Store {
	s := &Store{start: time.Now()}
	s.ptr.Store(&initial)
	return s
}

func (s *Store) Get() Config { return *s.ptr.Load() }

func (s *Store) Set(c Config) { s.ptr.Store(&c) }

// InSlowStart reports whether the process is still within its configured
// slow-start window.
func (s *Store) InSlowStart(c Config) bool {
	return c.SlowStartFor > 0 && time.Since(s.start) < c.SlowStartFor
}

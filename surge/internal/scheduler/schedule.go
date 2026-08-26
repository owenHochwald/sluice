// Package scheduler generates and drives an open-loop request schedule: work
// is issued at fixed intended times regardless of whether prior work has
// completed, so a stalled target shows up as tail latency instead of being
// silently absorbed by the generator waiting for it (docs/SPEC.md LG-U-02).
package scheduler

import (
	"context"
	"sync"
	"time"
)

// Tick is one scheduled unit of work: Seq is its position in the schedule,
// Intended is the wall-clock time it should have been dispatched at.
type Tick struct {
	Seq      int
	Intended time.Time
}

// Generate returns the fixed schedule of intended send times for rate
// connections/sec over duration, starting at start. Pure and deterministic
// so it's testable without sleeping.
func Generate(start time.Time, rate float64, duration time.Duration) []Tick {
	if rate <= 0 || duration <= 0 {
		return nil
	}
	interval := time.Duration(float64(time.Second) / rate)
	if interval <= 0 {
		interval = time.Nanosecond
	}
	n := int(duration / interval)
	ticks := make([]Tick, 0, n+1)
	for i := 0; ; i++ {
		offset := time.Duration(i) * interval
		if offset >= duration {
			break
		}
		ticks = append(ticks, Tick{Seq: i, Intended: start.Add(offset)})
	}
	return ticks
}

// Result summarizes how the schedule was actually driven.
type Result struct {
	Start       time.Time
	End         time.Time
	IssuedTicks int
	LaggedTicks int // ticks dispatched more than LagThreshold late
	MaxLag      time.Duration
	Cancelled   bool
}

// Scheduler drives a Tick schedule, bounding concurrent in-flight work so a
// stalled target can't grow goroutines without limit.
type Scheduler struct {
	// MaxInFlight caps concurrent work goroutines. Acquiring this semaphore
	// is what turns target-side stalls into observable dispatch lag: if the
	// pool is saturated, the next tick's dispatch blocks past its intended
	// time, which is exactly the signal LG-X-01 needs.
	MaxInFlight int
	// LagThreshold is how late a dispatch has to be before it counts as
	// "lagged" for LG-X-01's rate-limited-by-generator determination.
	LagThreshold time.Duration
}

// Run drives ticks in order, calling work(ctx, tick.Intended) for each one
// in its own goroutine once its intended time arrives (or immediately, if
// already past). Blocks until every dispatched goroutine returns or ctx is
// cancelled.
func (s *Scheduler) Run(ctx context.Context, ticks []Tick, work func(ctx context.Context, intended time.Time)) Result {
	res := Result{Start: time.Now()}
	if len(ticks) == 0 {
		res.End = time.Now()
		return res
	}

	maxInFlight := s.MaxInFlight
	if maxInFlight <= 0 {
		maxInFlight = 1
	}
	sem := make(chan struct{}, maxInFlight)
	var wg sync.WaitGroup

	for _, tick := range ticks {
		if ctx.Err() != nil {
			res.Cancelled = true
			break
		}

		if d := time.Until(tick.Intended); d > 0 {
			timer := time.NewTimer(d)
			select {
			case <-timer.C:
			case <-ctx.Done():
				timer.Stop()
				res.Cancelled = true
			}
			if res.Cancelled {
				break
			}
		}

		select {
		case sem <- struct{}{}:
		case <-ctx.Done():
			res.Cancelled = true
		}
		if res.Cancelled {
			break
		}

		lag := time.Since(tick.Intended)
		if lag > s.LagThreshold {
			res.LaggedTicks++
			if lag > res.MaxLag {
				res.MaxLag = lag
			}
		}
		res.IssuedTicks++

		wg.Add(1)
		go func(intended time.Time) {
			defer wg.Done()
			defer func() { <-sem }()
			work(ctx, intended)
		}(tick.Intended)
	}

	wg.Wait()
	res.End = time.Now()
	return res
}

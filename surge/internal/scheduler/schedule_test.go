package scheduler

import (
	"context"
	"sync/atomic"
	"testing"
	"time"
)

func TestGenerate_SpacingAndCount(t *testing.T) {
	tests := []struct {
		name     string
		rate     float64
		duration time.Duration
		wantN    int
	}{
		{"10 per sec for 1s", 10, time.Second, 10},
		{"100 per sec for 100ms", 100, 100 * time.Millisecond, 10},
		{"1 per sec for 3s", 1, 3 * time.Second, 3},
	}
	start := time.Unix(0, 0)
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			ticks := Generate(start, tt.rate, tt.duration)
			if len(ticks) != tt.wantN {
				t.Fatalf("got %d ticks, want %d", len(ticks), tt.wantN)
			}
			interval := time.Duration(float64(time.Second) / tt.rate)
			for i, tick := range ticks {
				if tick.Seq != i {
					t.Fatalf("tick[%d].Seq = %d", i, tick.Seq)
				}
				want := start.Add(time.Duration(i) * interval)
				if !tick.Intended.Equal(want) {
					t.Fatalf("tick[%d].Intended = %v, want %v", i, tick.Intended, want)
				}
			}
		})
	}
}

func TestGenerate_ZeroOrNegativeInputs(t *testing.T) {
	start := time.Now()
	if got := Generate(start, 0, time.Second); got != nil {
		t.Fatalf("rate=0: got %v, want nil", got)
	}
	if got := Generate(start, 10, 0); got != nil {
		t.Fatalf("duration=0: got %v, want nil", got)
	}
	if got := Generate(start, -1, time.Second); got != nil {
		t.Fatalf("negative rate: got %v, want nil", got)
	}
}

func TestScheduler_Run_IssuesAllTicksAndCallsWork(t *testing.T) {
	ticks := Generate(time.Now(), 1000, 50*time.Millisecond) // ~50 ticks, fast
	var calls int64
	s := &Scheduler{MaxInFlight: 100, LagThreshold: 100 * time.Millisecond}

	res := s.Run(context.Background(), ticks, func(ctx context.Context, intended time.Time) {
		atomic.AddInt64(&calls, 1)
	})

	if int(calls) != len(ticks) {
		t.Fatalf("work called %d times, want %d", calls, len(ticks))
	}
	if res.IssuedTicks != len(ticks) {
		t.Fatalf("IssuedTicks = %d, want %d", res.IssuedTicks, len(ticks))
	}
	if res.Cancelled {
		t.Fatal("expected Cancelled = false")
	}
}

func TestScheduler_Run_RespectsMaxInFlight(t *testing.T) {
	ticks := Generate(time.Now(), 2000, 20*time.Millisecond)
	var inFlight, maxObserved int64
	s := &Scheduler{MaxInFlight: 5, LagThreshold: time.Second}

	s.Run(context.Background(), ticks, func(ctx context.Context, intended time.Time) {
		n := atomic.AddInt64(&inFlight, 1)
		for {
			old := atomic.LoadInt64(&maxObserved)
			if n <= old || atomic.CompareAndSwapInt64(&maxObserved, old, n) {
				break
			}
		}
		time.Sleep(2 * time.Millisecond)
		atomic.AddInt64(&inFlight, -1)
	})

	if maxObserved > 5 {
		t.Fatalf("observed %d concurrent in-flight, want <= 5", maxObserved)
	}
}

func TestScheduler_Run_CancelledContextStopsEarly(t *testing.T) {
	ticks := Generate(time.Now().Add(time.Hour), 10, time.Second) // all far in the future
	ctx, cancel := context.WithCancel(context.Background())
	cancel()

	s := &Scheduler{MaxInFlight: 10, LagThreshold: time.Second}
	res := s.Run(ctx, ticks, func(ctx context.Context, intended time.Time) {
		t.Fatal("work should not be called when ctx is already cancelled")
	})

	if !res.Cancelled {
		t.Fatal("expected Cancelled = true")
	}
	if res.IssuedTicks != 0 {
		t.Fatalf("IssuedTicks = %d, want 0", res.IssuedTicks)
	}
}

func TestScheduler_Run_EmptySchedule(t *testing.T) {
	s := &Scheduler{MaxInFlight: 10, LagThreshold: time.Second}
	res := s.Run(context.Background(), nil, func(ctx context.Context, intended time.Time) {
		t.Fatal("work should not be called for an empty schedule")
	})
	if res.IssuedTicks != 0 || res.Cancelled {
		t.Fatalf("got %+v, want zero-value result", res)
	}
}

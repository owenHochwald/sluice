// Package run orchestrates one surge benchmark: schedule ticks, drive
// workload attempts, fold outcomes into the histogram and instance counter,
// and assemble the final RunResult.
package run

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"log/slog"
	"os"
	"path/filepath"
	"sync/atomic"
	"time"

	"github.com/owenhochwald/sluice/surge/internal/report"
	"github.com/owenhochwald/sluice/surge/internal/scheduler"
	"github.com/owenhochwald/sluice/surge/internal/workload"
)

// rateLimitedFraction is the fraction of ticks that must have dispatched
// late (beyond LagThreshold) before a run is flagged rate-limited-by-generator
// rather than target-limited (docs/SPEC.md LG-X-01).
const rateLimitedFraction = 0.01

// Options parameterizes one benchmark run.
type Options struct {
	Target       string
	Rate         float64
	Duration     time.Duration
	PayloadSize  int
	MaxInFlight  int
	DialTimeout  time.Duration
	IOTimeout    time.Duration
	LagThreshold time.Duration
	ReplicaID    string
	// ResultsDir, if set, is where summary.csv, latency_percentiles.csv,
	// result.json, and histogram.b64 are written at the end of the run.
	ResultsDir string
	Logger     *slog.Logger
}

// Execute runs one benchmark to completion (or until ctx is cancelled) and
// returns the assembled result. Respects cancellation by returning whatever
// partial result has accumulated, marked Cancelled, rather than discarding it.
func Execute(ctx context.Context, opts Options) report.RunResult {
	logger := opts.Logger
	if logger == nil {
		logger = slog.Default()
	}

	payload := make([]byte, opts.PayloadSize)
	for i := range payload {
		payload[i] = byte('a' + i%26)
	}

	rec := report.NewRecorder()
	ids := report.NewIDCounter()
	var errCount, mismatchCount int64

	ticks := scheduler.Generate(time.Now(), opts.Rate, opts.Duration)
	sch := &scheduler.Scheduler{MaxInFlight: opts.MaxInFlight, LagThreshold: opts.LagThreshold}

	wcfg := workload.Config{
		Target:      opts.Target,
		PayloadSize: opts.PayloadSize,
		DialTimeout: opts.DialTimeout,
		IOTimeout:   opts.IOTimeout,
	}

	schedResult := sch.Run(ctx, ticks, func(ctx context.Context, intended time.Time) {
		out := workload.Do(ctx, wcfg, intended, payload)
		if out.Err != nil {
			atomic.AddInt64(&errCount, 1)
			logger.Debug("workload attempt failed", "error", out.Err)
			return
		}
		if out.PayloadMismatch {
			atomic.AddInt64(&mismatchCount, 1)
		}
		rec.Record(out.Latency)
		ids.Add(out.InstanceID)
	})

	result := assemble(opts, schedResult, rec, ids, errCount, mismatchCount, logger)

	if opts.ResultsDir != "" {
		if err := writeArtifacts(opts.ResultsDir, result, rec); err != nil {
			logger.Error("write result artifacts", "dir", opts.ResultsDir, "error", err)
		}
	}

	return result
}

func assemble(
	opts Options,
	sched scheduler.Result,
	rec *report.Recorder,
	ids *report.IDCounter,
	errCount, mismatchCount int64,
	logger *slog.Logger,
) report.RunResult {
	elapsed := sched.End.Sub(sched.Start)
	var achievedRate float64
	if elapsed > 0 {
		achievedRate = float64(sched.IssuedTicks) / elapsed.Seconds()
	}

	totalTicks := sched.IssuedTicks
	rateLimited := totalTicks > 0 && float64(sched.LaggedTicks)/float64(totalTicks) > rateLimitedFraction

	encoded, err := rec.EncodeCompressed()
	if err != nil {
		logger.Error("encode histogram", "error", err)
	}

	return report.RunResult{
		ReplicaID:              opts.ReplicaID,
		Target:                 opts.Target,
		ConfiguredRate:         opts.Rate,
		AchievedRate:           achievedRate,
		RateLimited:            rateLimited,
		Duration:               elapsed,
		Cancelled:              sched.Cancelled,
		TotalIssued:            int64(sched.IssuedTicks),
		TotalErrors:            errCount,
		TotalPayloadMismatches: mismatchCount,
		Percentiles:            rec.Snapshot(),
		InstanceDistribution:   ids.Snapshot(),
		HistogramB64:           encoded,
	}
}

func writeArtifacts(dir string, result report.RunResult, rec *report.Recorder) error {
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return fmt.Errorf("mkdir results dir: %w", err)
	}

	return errors.Join(
		writeFile(filepath.Join(dir, "summary.csv"), func(f *os.File) error {
			return report.WriteSummaryCSV(f, result)
		}),
		writeFile(filepath.Join(dir, "latency_percentiles.csv"), func(f *os.File) error {
			return report.WritePercentileCSV(f, rec)
		}),
		writeFile(filepath.Join(dir, "result.json"), func(f *os.File) error {
			enc := json.NewEncoder(f)
			enc.SetIndent("", "  ")
			return enc.Encode(result)
		}),
		writeFile(filepath.Join(dir, "histogram.b64"), func(f *os.File) error {
			_, err := f.WriteString(result.HistogramB64)
			return err
		}),
	)
}

func writeFile(path string, write func(*os.File) error) error {
	f, err := os.Create(path)
	if err != nil {
		return fmt.Errorf("create %s: %w", path, err)
	}
	writeErr := write(f)
	closeErr := f.Close()
	if writeErr != nil {
		return fmt.Errorf("write %s: %w", path, writeErr)
	}
	if closeErr != nil {
		return fmt.Errorf("close %s: %w", path, closeErr)
	}
	return nil
}

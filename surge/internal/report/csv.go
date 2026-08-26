package report

import (
	"encoding/csv"
	"fmt"
	"io"
)

// summaryCSVHeader is one row per run, meant to be concatenated across runs
// and replicas for rate-vs-latency plots (docs/SPEC.md LG-U-07).
var summaryCSVHeader = []string{
	"replica_id", "target", "configured_rate", "achieved_rate", "rate_limited",
	"duration_s", "total_issued", "total_errors", "total_payload_mismatches",
	"p50_us", "p95_us", "p99_us", "p999_us", "max_us",
}

// WriteSummaryCSV writes r as a single CSV row (with header) suitable for
// appending to a shared results file across runs.
func WriteSummaryCSV(w io.Writer, r RunResult) error {
	cw := csv.NewWriter(w)
	if err := cw.Write(summaryCSVHeader); err != nil {
		return fmt.Errorf("write summary csv header: %w", err)
	}
	row := []string{
		r.ReplicaID,
		r.Target,
		fmt.Sprintf("%g", r.ConfiguredRate),
		fmt.Sprintf("%g", r.AchievedRate),
		fmt.Sprintf("%t", r.RateLimited),
		fmt.Sprintf("%g", r.Duration.Seconds()),
		fmt.Sprintf("%d", r.TotalIssued),
		fmt.Sprintf("%d", r.TotalErrors),
		fmt.Sprintf("%d", r.TotalPayloadMismatches),
		fmt.Sprintf("%d", r.Percentiles.P50.Microseconds()),
		fmt.Sprintf("%d", r.Percentiles.P95.Microseconds()),
		fmt.Sprintf("%d", r.Percentiles.P99.Microseconds()),
		fmt.Sprintf("%d", r.Percentiles.P999.Microseconds()),
		fmt.Sprintf("%d", r.Percentiles.Max.Microseconds()),
	}
	if err := cw.Write(row); err != nil {
		return fmt.Errorf("write summary csv row: %w", err)
	}
	cw.Flush()
	return cw.Error()
}

// percentilePoints is the standard curve for a log-scale latency plot: dense
// near the tail, where the interesting behavior is.
var percentilePoints = []float64{10, 25, 50, 75, 90, 95, 99, 99.9, 99.99, 100}

// WritePercentileCSV writes the percentile curve of rec as CSV
// (percentile,value_us), for plotting.
func WritePercentileCSV(w io.Writer, rec *Recorder) error {
	cw := csv.NewWriter(w)
	if err := cw.Write([]string{"percentile", "value_us"}); err != nil {
		return fmt.Errorf("write percentile csv header: %w", err)
	}
	for _, p := range percentilePoints {
		row := []string{fmt.Sprintf("%g", p), fmt.Sprintf("%d", rec.ValueAtPercentile(p))}
		if err := cw.Write(row); err != nil {
			return fmt.Errorf("write percentile csv row: %w", err)
		}
	}
	cw.Flush()
	return cw.Error()
}

// Package report turns raw workload outcomes into the artifacts surge
// exposes: an HDR latency histogram, a per-backend connection count, and the
// summary/CSV/JSON views over both.
package report

import (
	"encoding/base64"
	"fmt"
	"sync"
	"time"

	hdrhistogram "github.com/HdrHistogram/hdrhistogram-go"
)

// Values are recorded in microseconds: 1µs lowest, 10 minutes highest, 3
// significant figures. This unit choice is the one place it needs stating —
// everywhere else that reads percentiles back converts through
// time.Duration and never touches raw histogram units directly.
const (
	lowestTrackableMicros  = 1
	highestTrackableMicros = 600_000_000 // 10 minutes
	significantFigures     = 3
)

// Percentiles is a snapshot of the standard latency percentiles surge
// reports (docs/SPEC.md LG-U-04).
type Percentiles struct {
	P50  time.Duration
	P95  time.Duration
	P99  time.Duration
	P999 time.Duration
	Max  time.Duration
}

// Recorder is a concurrency-safe HDR histogram of latencies. The underlying
// hdrhistogram.Histogram isn't documented as safe for concurrent use, so
// every access goes through the mutex.
type Recorder struct {
	mu   sync.Mutex
	hist *hdrhistogram.Histogram
}

func NewRecorder() *Recorder {
	return &Recorder{hist: hdrhistogram.New(lowestTrackableMicros, highestTrackableMicros, significantFigures)}
}

// Record adds one latency sample.
func (r *Recorder) Record(d time.Duration) {
	r.mu.Lock()
	defer r.mu.Unlock()
	_ = r.hist.RecordValue(d.Microseconds()) // out-of-range values are clamped by the library, never fatal to a benchmark run
}

// Snapshot returns the current percentiles.
func (r *Recorder) Snapshot() Percentiles {
	r.mu.Lock()
	defer r.mu.Unlock()
	return Percentiles{
		P50:  time.Duration(r.hist.ValueAtPercentile(50)) * time.Microsecond,
		P95:  time.Duration(r.hist.ValueAtPercentile(95)) * time.Microsecond,
		P99:  time.Duration(r.hist.ValueAtPercentile(99)) * time.Microsecond,
		P999: time.Duration(r.hist.ValueAtPercentile(99.9)) * time.Microsecond,
		Max:  time.Duration(r.hist.Max()) * time.Microsecond,
	}
}

// EncodeCompressed returns the HDR V2 compressed encoding of the histogram,
// base64-encoded. This is docs/SPEC.md LG-E-01's mergeable encoding: decode
// with hdrhistogram.Decode and combine with Histogram.Merge, byte-exact per
// HDR's own spec, so multiple surge replicas' independent runs can be
// combined into one picture without loss.
func (r *Recorder) EncodeCompressed() (string, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	buf, err := r.hist.Encode(hdrhistogram.V2CompressedEncodingCookieBase)
	if err != nil {
		return "", fmt.Errorf("encode histogram: %w", err)
	}
	return base64.StdEncoding.EncodeToString(buf), nil
}

// ValueAtPercentile exposes the raw microsecond value at an arbitrary
// percentile, for building a percentile curve (see csv.go).
func (r *Recorder) ValueAtPercentile(p float64) int64 {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.hist.ValueAtPercentile(p)
}

// DecodeAndMerge decodes one or more base64 HDR V2 encodings (as produced by
// EncodeCompressed) and merges them into a single histogram. Building block
// for combining multiple surge replicas' results after the fact; surge
// itself does not call this — each replica exposes its own encoding via
// /histogram for a human or a follow-up tool to combine.
func DecodeAndMerge(encoded ...string) (*hdrhistogram.Histogram, error) {
	if len(encoded) == 0 {
		return nil, fmt.Errorf("no histograms to merge")
	}
	merged, err := decodeOne(encoded[0])
	if err != nil {
		return nil, err
	}
	for _, e := range encoded[1:] {
		h, err := decodeOne(e)
		if err != nil {
			return nil, err
		}
		merged.Merge(h)
	}
	return merged, nil
}

func decodeOne(encoded string) (*hdrhistogram.Histogram, error) {
	buf, err := base64.StdEncoding.DecodeString(encoded)
	if err != nil {
		return nil, fmt.Errorf("base64 decode histogram: %w", err)
	}
	h, err := hdrhistogram.Decode(buf)
	if err != nil {
		return nil, fmt.Errorf("decode histogram: %w", err)
	}
	return h, nil
}

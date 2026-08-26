package report

import "time"

// RunResult is the complete outcome of one surge benchmark run: what /results
// serves as JSON, and what the CSV/histogram artifacts are derived from.
type RunResult struct {
	ReplicaID string `json:"replicaId"`
	Target    string `json:"target"`

	ConfiguredRate float64       `json:"configuredRate"`
	AchievedRate   float64       `json:"achievedRate"`
	RateLimited    bool          `json:"rateLimited"` // docs/SPEC.md LG-X-01
	Duration       time.Duration `json:"durationNs"`
	Cancelled      bool          `json:"cancelled"` // true if the run was cut short by shutdown, not by completing its schedule

	TotalIssued            int64 `json:"totalIssued"`
	TotalErrors            int64 `json:"totalErrors"`
	TotalPayloadMismatches int64 `json:"totalPayloadMismatches"`

	Percentiles          Percentiles      `json:"percentiles"`
	InstanceDistribution map[string]int64 `json:"instanceDistribution"` // docs/SPEC.md LG-U-05

	// HistogramB64 is the HDR V2 compressed encoding of the latency
	// histogram (docs/SPEC.md LG-E-01), for merging across replicas.
	HistogramB64 string `json:"histogramB64"`
}

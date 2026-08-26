// Package admin serves surge's results over HTTP: health, JSON, CSV, and the
// raw HDR histogram encoding, so a benchmark pod stays inspectable long
// after its run has finished (docs/SPEC.md LG-U-06 — this is what lets
// surge live as a Deployment instead of a one-shot Job).
package admin

import (
	"encoding/json"
	"net/http"
	"sync/atomic"

	"github.com/owenhochwald/sluice/surge/internal/report"
)

// Server serves the current run result, if any, from a shared pointer that
// main.go populates once the benchmark completes.
type Server struct {
	Result *atomic.Pointer[report.RunResult]
}

func (s *Server) Handler() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("/healthz", func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusOK)
	})
	mux.HandleFunc("/results", s.handleResultsJSON)
	mux.HandleFunc("/results.csv", s.handleResultsCSV)
	mux.HandleFunc("/histogram", s.handleHistogram)
	return mux
}

func (s *Server) current() (report.RunResult, bool) {
	p := s.Result.Load()
	if p == nil {
		return report.RunResult{}, false
	}
	return *p, true
}

func (s *Server) handleResultsJSON(w http.ResponseWriter, r *http.Request) {
	result, ok := s.current()
	if !ok {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusAccepted)
		_ = json.NewEncoder(w).Encode(map[string]string{"status": "running"})
		return
	}
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(result)
}

func (s *Server) handleResultsCSV(w http.ResponseWriter, r *http.Request) {
	result, ok := s.current()
	if !ok {
		http.Error(w, "benchmark still running", http.StatusAccepted)
		return
	}
	w.Header().Set("Content-Type", "text/csv")
	if err := report.WriteSummaryCSV(w, result); err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
	}
}

func (s *Server) handleHistogram(w http.ResponseWriter, r *http.Request) {
	result, ok := s.current()
	if !ok {
		http.Error(w, "benchmark still running", http.StatusAccepted)
		return
	}
	w.Header().Set("Content-Type", "text/plain")
	_, _ = w.Write([]byte(result.HistogramB64))
}

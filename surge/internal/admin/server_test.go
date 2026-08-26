package admin

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"sync/atomic"
	"testing"

	"github.com/owenhochwald/sluice/surge/internal/report"
)

func TestHandler_Healthz(t *testing.T) {
	srv := httptest.NewServer((&Server{Result: &atomic.Pointer[report.RunResult]{}}).Handler())
	defer srv.Close()

	resp, err := http.Get(srv.URL + "/healthz")
	if err != nil {
		t.Fatalf("GET /healthz: %v", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("status = %d, want 200", resp.StatusCode)
	}
}

func TestHandler_ResultsBeforeRunCompletes(t *testing.T) {
	srv := httptest.NewServer((&Server{Result: &atomic.Pointer[report.RunResult]{}}).Handler())
	defer srv.Close()

	resp, err := http.Get(srv.URL + "/results")
	if err != nil {
		t.Fatalf("GET /results: %v", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusAccepted {
		t.Fatalf("status = %d, want 202", resp.StatusCode)
	}
}

func TestHandler_ResultsAfterRunCompletes(t *testing.T) {
	var ptr atomic.Pointer[report.RunResult]
	ptr.Store(&report.RunResult{ReplicaID: "surge-0", TotalIssued: 42, HistogramB64: "abc123"})
	srv := httptest.NewServer((&Server{Result: &ptr}).Handler())
	defer srv.Close()

	resp, err := http.Get(srv.URL + "/results")
	if err != nil {
		t.Fatalf("GET /results: %v", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("status = %d, want 200", resp.StatusCode)
	}
	var got report.RunResult
	if err := json.NewDecoder(resp.Body).Decode(&got); err != nil {
		t.Fatalf("decode: %v", err)
	}
	if got.ReplicaID != "surge-0" || got.TotalIssued != 42 {
		t.Fatalf("got %+v", got)
	}

	csvResp, err := http.Get(srv.URL + "/results.csv")
	if err != nil {
		t.Fatalf("GET /results.csv: %v", err)
	}
	defer csvResp.Body.Close()
	if csvResp.StatusCode != http.StatusOK {
		t.Fatalf("csv status = %d, want 200", csvResp.StatusCode)
	}

	histResp, err := http.Get(srv.URL + "/histogram")
	if err != nil {
		t.Fatalf("GET /histogram: %v", err)
	}
	defer histResp.Body.Close()
	if histResp.StatusCode != http.StatusOK {
		t.Fatalf("histogram status = %d, want 200", histResp.StatusCode)
	}
}

func TestHandler_HistogramBeforeRunCompletes(t *testing.T) {
	srv := httptest.NewServer((&Server{Result: &atomic.Pointer[report.RunResult]{}}).Handler())
	defer srv.Close()

	resp, err := http.Get(srv.URL + "/histogram")
	if err != nil {
		t.Fatalf("GET /histogram: %v", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusAccepted {
		t.Fatalf("status = %d, want 202", resp.StatusCode)
	}
}

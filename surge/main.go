// Command surge is the load generator: open-loop TCP connection load
// against a target address, HDR-histogram latency reporting, and per-backend
// instance attribution. docs/SPEC.md §9.
//
// surge runs its configured benchmark once on startup, then keeps its admin
// HTTP server serving the result (/results, /results.csv, /histogram)
// indefinitely, so it stays a long-running Deployment rather than a one-shot
// Job — matching docs/SPEC.md LG-U-06 literally.
package main

import (
	"context"
	"flag"
	"log/slog"
	"net/http"
	"os"
	"os/signal"
	"sync/atomic"
	"syscall"
	"time"

	"github.com/owenhochwald/sluice/surge/internal/admin"
	"github.com/owenhochwald/sluice/surge/internal/report"
	"github.com/owenhochwald/sluice/surge/internal/run"
)

func main() {
	var (
		target       = flag.String("target", "", "host:port of the echo target (required)")
		rate         = flag.Float64("rate", 100, "target connections/sec for this replica")
		duration     = flag.Duration("duration", 30*time.Second, "how long to run the benchmark")
		payloadSize  = flag.Int("payload-size", 64, "bytes written and echoed back per connection")
		maxInFlight  = flag.Int("max-inflight", 2000, "cap on concurrent in-flight connections")
		dialTimeout  = flag.Duration("dial-timeout", 2*time.Second, "per-connection dial timeout")
		ioTimeout    = flag.Duration("io-timeout", 5*time.Second, "per-connection read/write deadline")
		lagThreshold = flag.Duration("lag-threshold", 50*time.Millisecond, "dispatch lag beyond which a tick counts toward rate-limited detection")
		resultsDir   = flag.String("results-dir", "", "if set, write summary.csv/latency_percentiles.csv/result.json/histogram.b64 here")
		adminAddr    = flag.String("admin-listen", ":7002", "address the admin HTTP API binds to")
		id           = flag.String("id", "", "replica identifier for result attribution (default: hostname, i.e. pod name in k8s)")
	)
	flag.Parse()

	logger := slog.New(slog.NewTextHandler(os.Stderr, nil))

	if *target == "" {
		logger.Error("-target is required")
		os.Exit(1)
	}

	replicaID := *id
	if replicaID == "" {
		if h, err := os.Hostname(); err == nil {
			replicaID = h
		}
	}

	ctx, cancel := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer cancel()

	var resultPtr atomic.Pointer[report.RunResult]
	adminSrv := &admin.Server{Result: &resultPtr}
	httpSrv := &http.Server{Addr: *adminAddr, Handler: adminSrv.Handler()}

	go func() {
		logger.Info("admin API listening", "addr", *adminAddr)
		if err := httpSrv.ListenAndServe(); err != nil && err != http.ErrServerClosed {
			logger.Error("admin server stopped", "error", err)
		}
	}()

	logger.Info("starting benchmark", "target", *target, "rate", *rate, "duration", *duration, "replica", replicaID)
	result := run.Execute(ctx, run.Options{
		Target:       *target,
		Rate:         *rate,
		Duration:     *duration,
		PayloadSize:  *payloadSize,
		MaxInFlight:  *maxInFlight,
		DialTimeout:  *dialTimeout,
		IOTimeout:    *ioTimeout,
		LagThreshold: *lagThreshold,
		ReplicaID:    replicaID,
		ResultsDir:   *resultsDir,
		Logger:       logger,
	})
	resultPtr.Store(&result)

	logger.Info("benchmark complete, serving results",
		"issued", result.TotalIssued,
		"errors", result.TotalErrors,
		"achievedRate", result.AchievedRate,
		"rateLimited", result.RateLimited,
		"p50", result.Percentiles.P50,
		"p99", result.Percentiles.P99,
	)

	<-ctx.Done()
	logger.Info("shutting down")
	_ = httpSrv.Shutdown(context.Background())
}

// Command echod is the mock TCP backend: echoes bytes back, and can lie on
// command via its admin HTTP API (latency, errors, hangs, slow-start).
// docs/SPEC.md §8.
package main

import (
	"flag"
	"log/slog"
	"net"
	"net/http"
	"os"

	"github.com/owenhochwald/sluice/echod/internal/admin"
	"github.com/owenhochwald/sluice/echod/internal/echo"
	"github.com/owenhochwald/sluice/echod/internal/fault"
)

func main() {
	var (
		listenAddr = flag.String("listen", ":7000", "address the echo listener binds to")
		adminAddr  = flag.String("admin-listen", ":7001", "address the admin HTTP API binds to")
		id         = flag.String("id", "", "instance identifier emitted to clients (default: random)")
	)
	flag.Parse()

	logger := slog.New(slog.NewTextHandler(os.Stderr, nil))

	instanceID := *id
	if instanceID == "" {
		instanceID = echo.NewInstanceID()
	}

	lis, err := net.Listen("tcp", *listenAddr)
	if err != nil {
		logger.Error("listen", "addr", *listenAddr, "error", err)
		os.Exit(1)
	}

	store := fault.NewStore(fault.Config{})
	echoSrv := &echo.Server{Listener: lis, InstanceID: instanceID, Faults: store, Logger: logger}
	adminSrv := &admin.Server{Faults: store}

	go func() {
		logger.Info("admin API listening", "addr", *adminAddr)
		if err := http.ListenAndServe(*adminAddr, adminSrv.Handler()); err != nil {
			logger.Error("admin server stopped", "error", err)
		}
	}()

	logger.Info("echod listening", "addr", *listenAddr, "id", instanceID)
	if err := echoSrv.Serve(); err != nil {
		logger.Error("echo server stopped", "error", err)
		os.Exit(1)
	}
}

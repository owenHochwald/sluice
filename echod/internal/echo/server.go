// Package echo is the TCP side of echod: accept, optionally misbehave
// according to the live fault.Config, then echo bytes back verbatim.
// docs/SPEC.md §8.
package echo

import (
	cryptorand "crypto/rand"
	"encoding/hex"
	"fmt"
	"io"
	"log/slog"
	"math/rand/v2"
	"net"
	"time"

	"github.com/owenhochwald/sluice/echod/internal/fault"
)

// Server accepts TCP connections on Listener and echoes bytes back,
// applying whatever fault.Config is live at the moment each connection is
// accepted.
type Server struct {
	Listener   net.Listener
	InstanceID string
	Faults     *fault.Store
	Logger     *slog.Logger
}

// NewInstanceID returns a short random hex identifier suitable for
// distinguishing this echod instance from its replicas (BE-U-02).
func NewInstanceID() string {
	var b [4]byte
	_, _ = cryptorand.Read(b[:])
	return hex.EncodeToString(b[:])
}

// Serve accepts connections until Listener is closed.
func (s *Server) Serve() error {
	for {
		conn, err := s.Listener.Accept()
		if err != nil {
			return err
		}
		go s.handle(conn)
	}
}

func (s *Server) handle(conn net.Conn) {
	defer conn.Close()
	cfg := s.Faults.Get()

	if cfg.ErrorRate > 0 && rand.Float64() < cfg.ErrorRate { //nolint:gosec // fault injection, not security-sensitive
		return // BE-O-02: abort immediately, no ID line, no echo.
	}

	if cfg.Hang {
		// hold the connection open until the peer responds
		_, _ = io.Copy(io.Discard, conn)
		return
	}

	if _, err := fmt.Fprintf(conn, "ID %s\n", s.InstanceID); err != nil { // BE-U-02
		return
	}

	buf := make([]byte, 32*1024)
	for {
		n, err := conn.Read(buf)
		if n > 0 {
			s.delay(cfg)
			if _, werr := conn.Write(buf[:n]); werr != nil {
				return
			}
		}
		if err != nil {
			if err != io.EOF && s.Logger != nil {
				s.Logger.Debug("connection closed", "error", err)
			}
			return
		}
	}
}

// delay applies configured latency, plus slow-start extra latency for the
// configured window after process start (BE-O-01/04).
func (s *Server) delay(cfg fault.Config) {
	d := boundedRandom(cfg.LatencyMin, cfg.LatencyMax)
	if s.Faults.InSlowStart(cfg) {
		d += cfg.SlowStartExtra
	}
	if d > 0 {
		time.Sleep(d)
	}
}

func boundedRandom(min, max time.Duration) time.Duration {
	if max <= min {
		return min
	}
	return min + time.Duration(rand.Int64N(int64(max-min)))
}

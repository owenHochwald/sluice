package echo

import (
	"bufio"
	"net"
	"testing"
	"time"

	"github.com/owenhochwald/sluice/echod/internal/fault"
)

func startServer(t *testing.T, cfg fault.Config) (addr string, stop func()) {
	t.Helper()
	lis, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("listen: %v", err)
	}
	s := &Server{Listener: lis, InstanceID: "test-instance", Faults: fault.NewStore(cfg)}
	go s.Serve() //nolint:errcheck // Serve returns once Listener is closed, which is expected here
	return lis.Addr().String(), func() { lis.Close() }
}

func TestEcho_RoundTripsBytes(t *testing.T) {
	addr, stop := startServer(t, fault.Config{})
	defer stop()

	conn, err := net.Dial("tcp", addr)
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	defer conn.Close()

	r := bufio.NewReader(conn)
	idLine, err := r.ReadString('\n')
	if err != nil {
		t.Fatalf("read ID line: %v", err)
	}
	if idLine != "ID test-instance\n" {
		t.Fatalf("got ID line %q", idLine)
	}

	if _, err := conn.Write([]byte("hello\n")); err != nil {
		t.Fatalf("write: %v", err)
	}
	echoed, err := r.ReadString('\n')
	if err != nil {
		t.Fatalf("read echo: %v", err)
	}
	if echoed != "hello\n" {
		t.Fatalf("got %q, want %q", echoed, "hello\n")
	}
}

func TestEcho_ErrorRateOne_AlwaysAborts(t *testing.T) {
	addr, stop := startServer(t, fault.Config{ErrorRate: 1.0})
	defer stop()

	conn, err := net.Dial("tcp", addr)
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	defer conn.Close()

	conn.SetReadDeadline(time.Now().Add(time.Second))
	buf := make([]byte, 1)
	if _, err := conn.Read(buf); err == nil {
		t.Fatal("expected connection to be aborted, got data instead")
	}
}

func TestEcho_ErrorRateZero_NeverAborts(t *testing.T) {
	addr, stop := startServer(t, fault.Config{ErrorRate: 0})
	defer stop()

	conn, err := net.Dial("tcp", addr)
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	defer conn.Close()

	conn.SetReadDeadline(time.Now().Add(time.Second))
	r := bufio.NewReader(conn)
	if _, err := r.ReadString('\n'); err != nil {
		t.Fatalf("expected ID line, got error: %v", err)
	}
}

func TestEcho_Hang_NoResponse(t *testing.T) {
	addr, stop := startServer(t, fault.Config{Hang: true})
	defer stop()

	conn, err := net.Dial("tcp", addr)
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	defer conn.Close()

	conn.SetReadDeadline(time.Now().Add(200 * time.Millisecond))
	buf := make([]byte, 1)
	_, err = conn.Read(buf)
	if ne, ok := err.(net.Error); !ok || !ne.Timeout() {
		t.Fatalf("expected read timeout while hanging, got: %v", err)
	}
}

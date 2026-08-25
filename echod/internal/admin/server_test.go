package admin

import (
	"bufio"
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/owenhochwald/sluice/echod/internal/echo"
	"github.com/owenhochwald/sluice/echod/internal/fault"
)

// TestPostConfig_AppliesLiveWithoutRestart proves BE-E-01 directly: a
// connection accepted before the POST behaves under the old config, and one
// accepted after behaves under the new config, with no restart in between.
func TestPostConfig_AppliesLiveWithoutRestart(t *testing.T) {
	store := fault.NewStore(fault.Config{})
	lis, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("listen: %v", err)
	}
	defer lis.Close()
	echoSrv := &echo.Server{Listener: lis, InstanceID: "test", Faults: store}
	go echoSrv.Serve() //nolint:errcheck

	adminSrv := httptest.NewServer((&Server{Faults: store}).Handler())
	defer adminSrv.Close()

	// Before: default config, connection is served normally.
	conn1, err := net.Dial("tcp", lis.Addr().String())
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	defer conn1.Close()
	conn1.SetReadDeadline(time.Now().Add(time.Second))
	if _, err := bufio.NewReader(conn1).ReadString('\n'); err != nil {
		t.Fatalf("expected ID line before reconfigure, got: %v", err)
	}

	// Reconfigure to always abort, without restarting anything.
	resp, err := http.Post(adminSrv.URL+"/config", "application/json", strings.NewReader(`{"ErrorRate":1}`))
	if err != nil {
		t.Fatalf("POST /config: %v", err)
	}
	resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("POST /config status = %d", resp.StatusCode)
	}

	// After: the next connection is aborted.
	conn2, err := net.Dial("tcp", lis.Addr().String())
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	defer conn2.Close()
	conn2.SetReadDeadline(time.Now().Add(time.Second))
	buf := make([]byte, 1)
	if _, err := conn2.Read(buf); err == nil {
		t.Fatal("expected connection to be aborted after live reconfigure")
	}
}

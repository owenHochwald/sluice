package workload

import (
	"context"
	"strings"
	"testing"
	"time"

	"github.com/owenhochwald/sluice/surge/internal/testutil"
)

func TestDo_HappyPath(t *testing.T) {
	fake, err := testutil.StartFakeEcho("backend-1")
	if err != nil {
		t.Fatalf("start fake echo: %v", err)
	}
	defer fake.Close()

	cfg := Config{Target: fake.Addr(), DialTimeout: time.Second, IOTimeout: time.Second}
	payload := []byte("hello world")
	out := Do(context.Background(), cfg, time.Now(), payload)

	if out.Err != nil {
		t.Fatalf("unexpected error: %v", out.Err)
	}
	if out.InstanceID != "backend-1" {
		t.Fatalf("InstanceID = %q, want %q", out.InstanceID, "backend-1")
	}
	if out.PayloadMismatch {
		t.Fatal("expected payload to round-trip without mismatch")
	}
	if out.Latency <= 0 {
		t.Fatal("expected positive latency")
	}
}

func TestDo_Hang_TimesOut(t *testing.T) {
	fake, err := testutil.StartFakeEcho("backend-1")
	if err != nil {
		t.Fatalf("start fake echo: %v", err)
	}
	fake.Hang = true
	defer fake.Close()

	cfg := Config{Target: fake.Addr(), DialTimeout: time.Second, IOTimeout: 100 * time.Millisecond}
	out := Do(context.Background(), cfg, time.Now(), []byte("x"))

	if out.Err == nil {
		t.Fatal("expected a timeout error, got nil")
	}
}

func TestDo_Abort_ReportsErrorAndNoID(t *testing.T) {
	fake, err := testutil.StartFakeEcho("backend-1")
	if err != nil {
		t.Fatalf("start fake echo: %v", err)
	}
	fake.Abort = true
	defer fake.Close()

	cfg := Config{Target: fake.Addr(), DialTimeout: time.Second, IOTimeout: time.Second}
	out := Do(context.Background(), cfg, time.Now(), []byte("x"))

	if out.Err == nil {
		t.Fatal("expected an error for an aborted connection")
	}
	if out.InstanceID != "" {
		t.Fatalf("InstanceID = %q, want empty", out.InstanceID)
	}
}

func TestDo_DialFailure(t *testing.T) {
	cfg := Config{Target: "127.0.0.1:1", DialTimeout: 200 * time.Millisecond, IOTimeout: time.Second}
	out := Do(context.Background(), cfg, time.Now(), []byte("x"))
	if out.Err == nil {
		t.Fatal("expected a dial error")
	}
}

func TestParseIDLine(t *testing.T) {
	tests := []struct {
		name   string
		line   string
		wantID string
		wantOK bool
	}{
		{"well formed", "ID abcd1234\n", "abcd1234", true},
		{"crlf", "ID abcd1234\r\n", "abcd1234", true},
		{"missing prefix", "abcd1234\n", "", false},
		{"empty", "", "", false},
		{"just prefix, empty id", "ID \n", "", false},
		{"no newline", "ID abcd1234", "abcd1234", true},
		{"garbage", strings.Repeat("x", 10), "", false},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			id, ok := parseIDLine(tt.line)
			if ok != tt.wantOK || id != tt.wantID {
				t.Fatalf("parseIDLine(%q) = (%q, %v), want (%q, %v)", tt.line, id, ok, tt.wantID, tt.wantOK)
			}
		})
	}
}

// Package testutil provides a minimal in-process stand-in for echod's wire
// protocol, for surge's own unit tests. It cannot import echod/internal/echo
// (Go internal-package visibility is scoped to echod/), so this is a small
// dedicated fixture rather than the real thing — good enough to exercise
// surge's client-side handling of the happy path, a hang, and an abort.
package testutil

import (
	"fmt"
	"io"
	"net"
)

// FakeEcho is a tiny TCP server mirroring echod's protocol: on connect it
// writes "ID <id>\n" then echoes bytes back verbatim, unless configured to
// Hang (never write anything) or Abort (close immediately, no ID line).
type FakeEcho struct {
	Listener net.Listener
	ID       string
	Hang     bool
	Abort    bool
}

// StartFakeEcho starts a FakeEcho listening on an OS-assigned loopback port
// and serving in the background until Close is called.
func StartFakeEcho(id string) (*FakeEcho, error) {
	lis, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		return nil, err
	}
	f := &FakeEcho{Listener: lis, ID: id}
	go f.serve()
	return f, nil
}

func (f *FakeEcho) Addr() string { return f.Listener.Addr().String() }

func (f *FakeEcho) Close() error { return f.Listener.Close() }

func (f *FakeEcho) serve() {
	for {
		conn, err := f.Listener.Accept()
		if err != nil {
			return
		}
		go f.handle(conn)
	}
}

func (f *FakeEcho) handle(conn net.Conn) {
	defer conn.Close()

	if f.Abort {
		return
	}
	if f.Hang {
		_, _ = io.Copy(io.Discard, conn) // accept and never respond
		return
	}
	if _, err := fmt.Fprintf(conn, "ID %s\n", f.ID); err != nil {
		return
	}

	buf := make([]byte, 32*1024)
	for {
		n, err := conn.Read(buf)
		if n > 0 {
			if _, werr := conn.Write(buf[:n]); werr != nil {
				return
			}
		}
		if err != nil {
			return
		}
	}
}

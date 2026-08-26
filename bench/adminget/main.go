// Command adminget is a one-shot client for sluiced's Unix-domain admin
// socket: it dials the socket, sends a single command line, and copies the
// reply to stdout. It stands in for sluicectl in the benchmark harness so the
// admin surface (docs/SPEC.md DP-U-14) is exercised without a full CLI.
package main

import (
	"fmt"
	"io"
	"net"
	"os"
	"strings"
	"time"
)

func main() {
	if len(os.Args) < 3 {
		fmt.Fprintln(os.Stderr, "usage: adminget <socket-path> <command...>")
		os.Exit(2)
	}
	sock := os.Args[1]
	cmd := strings.Join(os.Args[2:], " ")

	conn, err := net.DialTimeout("unix", sock, 2*time.Second)
	if err != nil {
		fmt.Fprintf(os.Stderr, "adminget: dial %s: %v\n", sock, err)
		os.Exit(1)
	}
	defer conn.Close()

	_ = conn.SetDeadline(time.Now().Add(2 * time.Second))
	if _, err := io.WriteString(conn, cmd+"\n"); err != nil {
		fmt.Fprintf(os.Stderr, "adminget: write: %v\n", err)
		os.Exit(1)
	}
	if _, err := io.Copy(os.Stdout, conn); err != nil {
		fmt.Fprintf(os.Stderr, "adminget: read: %v\n", err)
		os.Exit(1)
	}
}

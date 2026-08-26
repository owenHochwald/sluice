package report

import (
	"maps"
	"sync"
)

// IDCounter tracks how many connections landed on each backend instance ID
// (docs/SPEC.md LG-U-05).
type IDCounter struct {
	mu     sync.Mutex
	counts map[string]int64
}

func NewIDCounter() *IDCounter {
	return &IDCounter{counts: make(map[string]int64)}
}

// Add records one connection attributed to id. A blank id (an aborted
// connection that never got an ID line) is ignored — it's counted in the
// error total elsewhere, not as an unattributed backend.
func (c *IDCounter) Add(id string) {
	if id == "" {
		return
	}
	c.mu.Lock()
	defer c.mu.Unlock()
	c.counts[id]++
}

// Snapshot returns a copy of the current per-instance counts.
func (c *IDCounter) Snapshot() map[string]int64 {
	c.mu.Lock()
	defer c.mu.Unlock()
	out := make(map[string]int64, len(c.counts))
	maps.Copy(out, c.counts)
	return out
}

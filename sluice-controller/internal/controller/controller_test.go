package controller

import (
	"testing"

	"github.com/owenhochwald/sluice/sluice-controller/internal/backend"
)

// fakePublisher records every Set() call so tests can assert on how many
// times (and with what) the controller decided to publish.
type fakePublisher struct {
	calls []call
}

type call struct {
	version  uint64
	backends []backend.Backend
}

func (f *fakePublisher) Set(version uint64, backends []backend.Backend) {
	f.calls = append(f.calls, call{version, backends})
}

func newTestController(pub Publisher) *Controller {
	return New(nil, "default", "svc", pub, nil)
}

func TestApply_PublishesOnChange(t *testing.T) {
	pub := &fakePublisher{}
	c := newTestController(pub)

	c.apply([]backend.Backend{{Address: "10.0.0.1:9000"}})

	if len(pub.calls) != 1 {
		t.Fatalf("got %d Set() calls, want 1", len(pub.calls))
	}
	if pub.calls[0].version != 1 {
		t.Fatalf("got version %d, want 1", pub.calls[0].version)
	}
}

func TestApply_NoPublishWhenUnchanged(t *testing.T) {
	pub := &fakePublisher{}
	c := newTestController(pub)

	set := []backend.Backend{{Address: "10.0.0.1:9000"}}
	c.apply(set)
	c.apply(set) // identical recomputed set

	if len(pub.calls) != 1 {
		t.Fatalf("got %d Set() calls, want 1 (second apply should be a no-op)", len(pub.calls))
	}
}

func TestApply_EmptySetIsDropped(t *testing.T) {
	pub := &fakePublisher{}
	c := newTestController(pub)

	c.apply([]backend.Backend{{Address: "10.0.0.1:9000"}})
	c.apply(nil) // computed set is empty: must not publish, must not clear c.last

	if len(pub.calls) != 1 {
		t.Fatalf("got %d Set() calls, want 1 (empty set must not publish)", len(pub.calls))
	}
}

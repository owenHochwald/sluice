// Package stream implements the gRPC ConfigStream service: the only channel
// through which the control plane talks to sluiced. See proto/sluice/v1/config.proto.
package stream

import (
	"sync"

	"github.com/owenhochwald/sluice/sluice-controller/internal/backend"
	pb "github.com/owenhochwald/sluice/sluice-controller/internal/genproto"
)

// Server implements pb.ConfigStreamServer. Set is the only mutation point:
// every reconcile that changes the backend set calls it once, which is also
// what makes this type testable without a real gRPC transport or informer.
type Server struct {
	pb.UnimplementedConfigStreamServer

	mu      sync.Mutex
	current *pb.BackendSet // nil until the first Set call
	subs    map[chan *pb.BackendSet]struct{}
}

func NewServer() *Server {
	return &Server{subs: make(map[chan *pb.BackendSet]struct{})}
}

// Set publishes a new backend set to the current subscriber and to every
// active Watch stream (SPEC.md CP-E-01). Callers must already have decided
// this is an actual change — Set does not diff.
func (s *Server) Set(version uint64, backends []backend.Backend) {
	bs := toProto(version, backends)

	s.mu.Lock()
	s.current = bs
	subs := make([]chan *pb.BackendSet, 0, len(s.subs))
	for ch := range s.subs {
		subs = append(subs, ch)
	}
	s.mu.Unlock()

	for _, ch := range subs {
		replaceLatest(ch, bs)
	}
}

// Watch streams the complete current backend set immediately on subscribe
// (SPEC.md CP-E-02 — a reconnecting data plane always gets full state, never
// a delta), then every subsequent Set().
func (s *Server) Watch(_ *pb.WatchRequest, stream pb.ConfigStream_WatchServer) error {
	ch := make(chan *pb.BackendSet, 1)

	s.mu.Lock()
	s.subs[ch] = struct{}{}
	current := s.current
	s.mu.Unlock()

	defer func() {
		s.mu.Lock()
		delete(s.subs, ch)
		s.mu.Unlock()
	}()

	if current != nil {
		if err := stream.Send(current); err != nil {
			return err
		}
	}

	for {
		select {
		case bs := <-ch:
			if err := stream.Send(bs); err != nil {
				return err
			}
		case <-stream.Context().Done():
			return stream.Context().Err()
		}
	}
}

func toProto(version uint64, backends []backend.Backend) *pb.BackendSet {
	out := &pb.BackendSet{Version: version, Backends: make([]*pb.Backend, len(backends))}
	for i, b := range backends {
		out.Backends[i] = &pb.Backend{Address: b.Address}
	}
	return out
}

// replaceLatest is a non-blocking send that keeps only the newest value.
// Safe because BackendSet is always full state (CP-U-03): a subscriber that
// misses an intermediate version and receives the next one is still
// correct, it just skips straight to current truth.
func replaceLatest(ch chan *pb.BackendSet, bs *pb.BackendSet) {
	for {
		select {
		case ch <- bs:
			return
		default:
			select {
			case <-ch:
			default:
			}
		}
	}
}

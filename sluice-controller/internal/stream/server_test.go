package stream

import (
	"context"
	"testing"
	"time"

	"github.com/owenhochwald/sluice/sluice-controller/internal/backend"
	pb "github.com/owenhochwald/sluice/sluice-controller/internal/genproto"
	"google.golang.org/grpc/metadata"
)

// fakeWatchStream is a minimal stand-in for pb.ConfigStream_WatchServer so
// Server.Watch can be tested without spinning up a real gRPC transport.
type fakeWatchStream struct {
	ctx context.Context
	out chan *pb.BackendSet
}

func (f *fakeWatchStream) Send(bs *pb.BackendSet) error {
	f.out <- bs
	return nil
}
func (f *fakeWatchStream) Context() context.Context     { return f.ctx }
func (f *fakeWatchStream) SetHeader(metadata.MD) error  { return nil }
func (f *fakeWatchStream) SendHeader(metadata.MD) error { return nil }
func (f *fakeWatchStream) SetTrailer(metadata.MD)       {}
func (f *fakeWatchStream) SendMsg(m any) error          { return f.Send(m.(*pb.BackendSet)) }
func (f *fakeWatchStream) RecvMsg(any) error            { return nil }

func newFakeStream() (*fakeWatchStream, context.CancelFunc) {
	ctx, cancel := context.WithCancel(context.Background())
	return &fakeWatchStream{ctx: ctx, out: make(chan *pb.BackendSet, 8)}, cancel
}

func TestWatch_ReceivesCurrentSetOnSubscribe(t *testing.T) {
	s := NewServer()
	s.Set(1, []backend.Backend{{Address: "10.0.0.1:9000"}})

	stream, cancel := newFakeStream()
	defer cancel()
	go func() { _ = s.Watch(&pb.WatchRequest{}, stream) }()

	select {
	case bs := <-stream.out:
		if bs.Version != 1 || len(bs.Backends) != 1 {
			t.Fatalf("got %+v, want version 1 with 1 backend", bs)
		}
	case <-time.After(time.Second):
		t.Fatal("timed out waiting for initial BackendSet")
	}
}

func TestWatch_ReceivesLaterSet(t *testing.T) {
	s := NewServer()

	stream, cancel := newFakeStream()
	defer cancel()
	go func() { _ = s.Watch(&pb.WatchRequest{}, stream) }()

	// Give Watch a moment to register its subscription before Set fires.
	time.Sleep(20 * time.Millisecond)
	s.Set(2, []backend.Backend{{Address: "10.0.0.2:9000"}})

	select {
	case bs := <-stream.out:
		if bs.Version != 2 {
			t.Fatalf("got version %d, want 2", bs.Version)
		}
	case <-time.After(time.Second):
		t.Fatal("timed out waiting for broadcast BackendSet")
	}
}

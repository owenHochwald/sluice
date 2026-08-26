package report

import (
	"reflect"
	"testing"
)

func TestIDCounter_CountsPerInstance(t *testing.T) {
	c := NewIDCounter()
	for _, id := range []string{"a", "a", "b", "a", "c", "b"} {
		c.Add(id)
	}

	want := map[string]int64{"a": 3, "b": 2, "c": 1}
	if got := c.Snapshot(); !reflect.DeepEqual(got, want) {
		t.Fatalf("got %v, want %v", got, want)
	}
}

func TestIDCounter_IgnoresBlankID(t *testing.T) {
	c := NewIDCounter()
	c.Add("")
	c.Add("a")
	c.Add("")

	want := map[string]int64{"a": 1}
	if got := c.Snapshot(); !reflect.DeepEqual(got, want) {
		t.Fatalf("got %v, want %v", got, want)
	}
}

func TestIDCounter_EmptySnapshot(t *testing.T) {
	c := NewIDCounter()
	got := c.Snapshot()
	if len(got) != 0 {
		t.Fatalf("got %v, want empty", got)
	}
}

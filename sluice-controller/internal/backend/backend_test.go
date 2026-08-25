package backend

import (
	"reflect"
	"testing"

	discoveryv1 "k8s.io/api/discovery/v1"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
)

func ptr[T any](v T) *T { return &v }

func port(n int32) discoveryv1.EndpointPort {
	return discoveryv1.EndpointPort{Port: ptr(n)}
}

func TestComputeBackendSet_ExcludesNotReady(t *testing.T) {
	slices := []discoveryv1.EndpointSlice{{
		ObjectMeta: metav1.ObjectMeta{Name: "s1"},
		Ports:      []discoveryv1.EndpointPort{port(9000)},
		Endpoints: []discoveryv1.Endpoint{
			{Addresses: []string{"10.0.0.1"}, Conditions: discoveryv1.EndpointConditions{Ready: ptr(true)}},
			{Addresses: []string{"10.0.0.2"}, Conditions: discoveryv1.EndpointConditions{Ready: ptr(false)}},
			{Addresses: []string{"10.0.0.3"}}, // unset Ready => treated as ready
		},
	}}

	got := ComputeBackendSet(slices)
	want := []Backend{{Address: "10.0.0.1:9000"}, {Address: "10.0.0.3:9000"}}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("got %v, want %v", got, want)
	}
}

func TestComputeBackendSet_DeterministicOrder(t *testing.T) {
	build := func(order []string) []discoveryv1.EndpointSlice {
		eps := make([]discoveryv1.Endpoint, len(order))
		for i, addr := range order {
			eps[i] = discoveryv1.Endpoint{Addresses: []string{addr}, Conditions: discoveryv1.EndpointConditions{Ready: ptr(true)}}
		}
		return []discoveryv1.EndpointSlice{{Ports: []discoveryv1.EndpointPort{port(80)}, Endpoints: eps}}
	}

	a := ComputeBackendSet(build([]string{"10.0.0.3", "10.0.0.1", "10.0.0.2"}))
	b := ComputeBackendSet(build([]string{"10.0.0.1", "10.0.0.2", "10.0.0.3"}))
	if !reflect.DeepEqual(a, b) {
		t.Fatalf("order depended on input order: %v vs %v", a, b)
	}
	if !sortedByAddress(a) {
		t.Fatalf("result not sorted: %v", a)
	}
}

func TestComputeBackendSet_EmptyInput(t *testing.T) {
	got := ComputeBackendSet(nil)
	if len(got) != 0 {
		t.Fatalf("got %v, want empty", got)
	}
}

func sortedByAddress(bs []Backend) bool {
	for i := 1; i < len(bs); i++ {
		if bs[i-1].Address > bs[i].Address {
			return false
		}
	}
	return true
}

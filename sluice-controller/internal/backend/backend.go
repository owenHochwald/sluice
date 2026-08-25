package backend

import (
	"net"
	"sort"
	"strconv"

	discoveryv1 "k8s.io/api/discovery/v1"
)

type Backend struct {
	Address string // "ip:port"
}

// filter unhealthy endpoints, combine address + port, dedup and then sort
func ComputeBackendSet(slices []discoveryv1.EndpointSlice) []Backend {
	seen := make(map[string]struct{})
	for _, slice := range slices {
		for _, ep := range slice.Endpoints {
			if !isReady(ep) {
				continue
			}
			for _, addr := range ep.Addresses {
				for _, port := range slice.Ports {
					if port.Port == nil {
						continue
					}
					// stdlib to handle this for us
					seen[net.JoinHostPort(addr, strconv.Itoa(int(*port.Port)))] = struct{}{}
				}
			}
		}
	}

	out := make([]Backend, 0, len(seen))
	for addr := range seen {
		out = append(out, Backend{Address: addr})
	}
	// sort to make sure we see the things in the same order instead of getting a false change
	sort.Slice(out, func(i, j int) bool { return out[i].Address < out[j].Address })
	return out
}

// kubernetes maintains readiness as a pointer
func isReady(ep discoveryv1.Endpoint) bool {
	if ep.Conditions.Ready == nil {
		return true
	}
	return *ep.Conditions.Ready
}

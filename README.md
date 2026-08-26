<div align="center">

# sluice

**A multi-core, shared-nothing TCP load balancer with a Kubernetes control
plane.**

</div>

`sluice` replaces what `kube-proxy` does for TCP traffic, but with the things a
real data plane needs that plain iptables/IPVS rules don't give you: error
awareness, power-of-two-choices, and ejection of bad backends.

`sluiced` runs one event loop per core with no shared state, routes connections
with a Maglev hash so backend changes don't disturb live traffic, and keeps
serving the last known-good config forever if its control plane disappears.

## Architecture

```
client traffic
      │
      ▼
 ┌─────────┐         config stream        ┌────────────────────┐
 │ sluiced │ ◄──────────────────────────  │ sluice-controller  │
 │  C++    │                              │        Go          │
 │ N loops │                              └─────────┬──────────┘
 └────┬────┘                                        │ watch
      │  admin socket ──► sluicectl (Go CLI)         ▼
      ▼                                      kube-apiserver
[echod] [echod] [echod]   ◄── load ──  [surge] [surge]
```

| Component           | Lang  | Role                                                             |
| ------------------- | ----- | ---------------------------------------------------------------- |
| `sluiced`           | C++20 | Data plane. Touches every byte, one event loop per core.         |
| `sluice-controller` | Go    | Control plane. Watches `EndpointSlice`, streams backend sets.    |
| `sluicectl`         | Go    | CLI over `sluiced`'s admin socket.                               |
| `echod`             | Go    | Mock TCP backend — can inject latency, errors, hangs on command. |
| `surge`             | Go    | Distributed load generator, HDR histograms.                      |

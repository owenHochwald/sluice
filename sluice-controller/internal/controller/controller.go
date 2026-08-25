package controller

import (
	"context"
	"log/slog"
	"sync/atomic"
	"time"

	discoveryv1 "k8s.io/api/discovery/v1"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/apimachinery/pkg/labels"
	"k8s.io/client-go/informers"
	"k8s.io/client-go/kubernetes"
	"k8s.io/client-go/tools/cache"

	"github.com/owenhochwald/sluice/sluice-controller/internal/backend"
)

type Publisher interface {
	Set(version uint64, backends []backend.Backend)
}

// Controller watches EndpointSlices for one Service and republishes the
// backend set whenever it actually changes.
type Controller struct {
	client    kubernetes.Interface
	namespace string
	service   string
	publisher Publisher
	logger    *slog.Logger

	informerFactory informers.SharedInformerFactory
	ready           atomic.Bool

	version uint64
	last    []backend.Backend
}

func New(client kubernetes.Interface, namespace, service string, publisher Publisher, logger *slog.Logger) *Controller {
	if logger == nil {
		logger = slog.Default()
	}
	return &Controller{client: client, namespace: namespace, service: service, publisher: publisher, logger: logger}
}

func (c *Controller) Ready() bool { return c.ready.Load() }

// Run watches EndpointSlices for the configured Service (list+watch via a
// shared informer, never poll — SPEC.md CP-U-02) and reconciles on every
// resync. Blocks until ctx is cancelled.
func (c *Controller) Run(ctx context.Context) error {
	selector := labels.Set{"kubernetes.io/service-name": c.service}.AsSelector()
	c.informerFactory = informers.NewSharedInformerFactoryWithOptions(
		c.client, 10*time.Minute,
		informers.WithNamespace(c.namespace),
		informers.WithTweakListOptions(func(opts *metav1.ListOptions) {
			opts.LabelSelector = selector.String()
		}),
	)

	informer := c.informerFactory.Discovery().V1().EndpointSlices().Informer()
	handler := cache.ResourceEventHandlerFuncs{
		AddFunc:    func(any) { c.reconcile() },
		UpdateFunc: func(any, any) { c.reconcile() },
		DeleteFunc: func(any) { c.reconcile() },
	}
	if _, err := informer.AddEventHandler(handler); err != nil {
		return err
	}

	c.informerFactory.Start(ctx.Done())
	if !cache.WaitForCacheSync(ctx.Done(), informer.HasSynced) {
		return ctx.Err()
	}
	c.ready.Store(true)
	c.reconcile()

	<-ctx.Done()
	return ctx.Err()
}

func (c *Controller) reconcile() {
	slices, err := c.informerFactory.Discovery().V1().EndpointSlices().Lister().EndpointSlices(c.namespace).List(labels.Everything())
	if err != nil {
		c.logger.Error("list endpointslices", "error", err)
		return
	}
	deref := make([]discoveryv1.EndpointSlice, len(slices))
	for i, s := range slices {
		deref[i] = *s
	}

	next := backend.ComputeBackendSet(deref)
	c.apply(next)
}

// apply is the pure diff/version/publish decision, split out from reconcile
// so it can be unit tested without an informer or a fake clientset.
func (c *Controller) apply(next []backend.Backend) {
	if len(next) == 0 {
		c.logger.Warn("computed backend set is empty, retaining previous set", "service", c.service)
		return // SPEC.md CP-X-02: never publish an empty set.
	}
	if equalBackends(next, c.last) {
		return // SPEC.md CP-X-01: identical set, no broadcast.
	}
	c.version++
	c.last = next
	c.publisher.Set(c.version, next) // SPEC.md CP-E-01.
}

func equalBackends(a, b []backend.Backend) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

// Command sluice-controller is the control plane: it watches EndpointSlices
// for one Service and streams the resulting backend set to every connected
// sluiced over gRPC. It never touches traffic. See docs/SPEC.md §6.
package main

import (
	"context"
	"flag"
	"fmt"
	"log/slog"
	"net"
	"net/http"
	"os"
	"os/signal"
	"syscall"

	"google.golang.org/grpc"
	"k8s.io/client-go/kubernetes"
	"k8s.io/client-go/rest"
	"k8s.io/client-go/tools/clientcmd"

	"github.com/owenhochwald/sluice/sluice-controller/internal/controller"
	pb "github.com/owenhochwald/sluice/sluice-controller/internal/genproto"
	"github.com/owenhochwald/sluice/sluice-controller/internal/stream"
)

func main() {
	var (
		kubeconfig = flag.String("kubeconfig", "", "path to kubeconfig (empty: in-cluster config)")
		namespace  = flag.String("namespace", "default", "namespace of the Service to watch")
		service    = flag.String("service", "", "name of the Service to watch (required)")
		grpcAddr   = flag.String("listen", ":9090", "address the ConfigStream gRPC server listens on")
		httpAddr   = flag.String("http", ":8080", "address the readiness endpoint listens on")
	)
	flag.Parse()

	logger := slog.New(slog.NewTextHandler(os.Stderr, nil))
	if *service == "" {
		logger.Error("-service is required")
		os.Exit(1)
	}

	client, err := buildClient(*kubeconfig)
	if err != nil {
		logger.Error("build kubernetes client", "error", err)
		os.Exit(1)
	}

	ctx, cancel := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer cancel()

	streamServer := stream.NewServer()
	ctrl := controller.New(client, *namespace, *service, streamServer, logger)

	grpcSrv := grpc.NewServer()
	pb.RegisterConfigStreamServer(grpcSrv, streamServer)
	lis, err := net.Listen("tcp", *grpcAddr)
	if err != nil {
		logger.Error("listen for gRPC", "addr", *grpcAddr, "error", err)
		os.Exit(1)
	}
	go func() {
		logger.Info("ConfigStream gRPC server listening", "addr", *grpcAddr)
		if err := grpcSrv.Serve(lis); err != nil {
			logger.Error("gRPC server stopped", "error", err)
		}
	}()

	httpSrv := &http.Server{Addr: *httpAddr, Handler: readyzHandler(ctrl)}
	go func() {
		logger.Info("readiness endpoint listening", "addr", *httpAddr)
		if err := httpSrv.ListenAndServe(); err != nil && err != http.ErrServerClosed {
			logger.Error("http server stopped", "error", err)
		}
	}()

	go func() {
		if err := ctrl.Run(ctx); err != nil && ctx.Err() == nil {
			logger.Error("controller stopped", "error", err)
		}
	}()

	<-ctx.Done()
	logger.Info("shutting down")
	grpcSrv.GracefulStop()
	_ = httpSrv.Shutdown(context.Background())
}

func readyzHandler(ctrl *controller.Controller) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if !ctrl.Ready() {
			w.WriteHeader(http.StatusServiceUnavailable)
			fmt.Fprintln(w, "informer cache not yet synced")
			return
		}
		w.WriteHeader(http.StatusOK)
		fmt.Fprintln(w, "ok")
	})
}

func buildClient(kubeconfig string) (kubernetes.Interface, error) {
	var cfg *rest.Config
	var err error
	if kubeconfig != "" {
		cfg, err = clientcmd.BuildConfigFromFlags("", kubeconfig)
	} else {
		cfg, err = rest.InClusterConfig()
	}
	if err != nil {
		return nil, err
	}
	return kubernetes.NewForConfig(cfg)
}

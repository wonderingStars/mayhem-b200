// SPDX-License-Identifier: GPL-2.0-or-later

// Part of mayhem-b200.
//
// Command mayhem-webgui runs mayhem-b200's web GUI as a standalone binary,
// driving any server that speaks the sdrlink wire protocol (PROTOCOL.md)
// over its control connection (default port 5960) and, once RX streaming
// starts, its binary IQ stream connection. It is not tied to sdrlink's own
// reference server implementation -- PROTOCOL.md is an open, documented
// protocol precisely so independent clients and servers can be written
// against it (see webgui/README.md).
package main

import (
	"context"
	"flag"
	"fmt"
	"log"
	"net"
	"net/http"
	"os"
	"os/signal"
	"strconv"
	"time"

	"mayhemb200/webgui/internal/sdrclient"
	"mayhemb200/webgui/internal/web"
	"mayhemb200/webgui/internal/webadapter"
)

func main() {
	serverAddr := flag.String("server", "127.0.0.1:5960", "host:port of the sdrlink-protocol control connection to drive")
	httpAddr := flag.String("http", ":8080", "address this web GUI's own HTTP server listens on")
	deviceArgs := flag.String("device", "", "device args to open at startup, e.g. driver=uhd (leave empty to pick and open a device from the GUI's device panel instead)")
	flag.Parse()

	logger := log.New(os.Stderr, "mayhem-webgui ", log.LstdFlags)

	streamAddr, err := deriveStreamAddr(*serverAddr)
	if err != nil {
		logger.Fatalf("%v", err)
	}

	dialCtx, dialCancel := context.WithTimeout(context.Background(), sdrclient.DefaultDialTimeout)
	client, err := sdrclient.Dial(dialCtx, *serverAddr)
	dialCancel()
	if err != nil {
		logger.Fatalf("connect to sdrlink server %s: %v", *serverAddr, err)
	}
	defer client.Close()

	helloCtx, helloCancel := context.WithTimeout(context.Background(), sdrclient.DefaultReplyTimeout)
	hello, err := client.Hello(helloCtx, sdrclient.ClientName)
	helloCancel()
	if err != nil {
		logger.Fatalf("hello to %s: %v", *serverAddr, err)
	}
	logger.Printf("connected to %s (%s, proto %d), session %s", *serverAddr, hello.Server, hello.Proto, hello.SessionID)

	backend := webadapter.NewBackend(client, *serverAddr, streamAddr, sdrclient.ClientName)

	if *deviceArgs != "" {
		// Opening a real radio can legitimately take 6-12s (PROTOCOL.md
		// `open`, USB enumeration + FPGA image load) -- DefaultOpenTimeout,
		// not DefaultReplyTimeout, is the right bound here. See
		// internal/sdrclient's DefaultOpenTimeout doc for why a short,
		// uniform timeout here is a known bug, not just over-caution.
		openCtx, openCancel := context.WithTimeout(context.Background(), sdrclient.DefaultOpenTimeout)
		dev, err := backend.OpenDevice(openCtx, *deviceArgs)
		openCancel()
		if err != nil {
			logger.Fatalf("open device %q: %v", *deviceArgs, err)
		}
		info := dev.Info()
		logger.Printf("opened device: driver=%s label=%q serial=%s", info.Driver, info.Label, info.Serial)
	}

	guiHandler := web.NewServer(backend)
	httpSrv := &http.Server{Addr: *httpAddr, Handler: guiHandler}

	httpErrCh := make(chan error, 1)
	go func() {
		if err := httpSrv.ListenAndServe(); err != nil && err != http.ErrServerClosed {
			httpErrCh <- err
		}
	}()
	logger.Printf("web GUI on http://%s/", displayAddr(*httpAddr))

	// Surface a bind failure (e.g. port already in use) promptly rather
	// than only discovering it when a browser fails to connect.
	select {
	case err := <-httpErrCh:
		logger.Fatalf("web GUI: %v", err)
	case <-time.After(200 * time.Millisecond):
	}

	sig := make(chan os.Signal, 1)
	signal.Notify(sig, os.Interrupt)
	<-sig
	logger.Print("shutting down")

	shutdownCtx, shutdownCancel := context.WithTimeout(context.Background(), 5*time.Second)
	if err := httpSrv.Shutdown(shutdownCtx); err != nil {
		logger.Printf("web GUI shutdown: %v", err)
	}
	shutdownCancel()

	if err := backend.CloseDevice(); err != nil {
		logger.Printf("close device: %v", err)
	}
	// client.Close() runs via defer above, tearing down the control
	// connection (and with it, per PROTOCOL.md section 4, any stream
	// connection and device the server still had open for this session).
}

// deriveStreamAddr applies PROTOCOL.md's reference-server convention for
// the IQ stream connection's port: the same host as the control
// connection, one port number higher (5960 control / 5961 stream by
// default). PROTOCOL.md has no command for a client to discover a
// non-default stream port any other way; this mirrors mayhem-b200's
// existing C++ NetworkRadio client (network_radio.cpp), which resolves the
// exact same ambiguity the exact same way.
func deriveStreamAddr(controlAddr string) (string, error) {
	host, portStr, err := net.SplitHostPort(controlAddr)
	if err != nil {
		return "", fmt.Errorf("-server %q: %w", controlAddr, err)
	}
	port, err := strconv.Atoi(portStr)
	if err != nil {
		return "", fmt.Errorf("-server %q: non-numeric port %q", controlAddr, portStr)
	}
	return net.JoinHostPort(host, strconv.Itoa(port+1)), nil
}

// displayAddr turns a listen address like ":8080" into something a user can
// paste into a browser, e.g. "127.0.0.1:8080".
func displayAddr(addr string) string {
	host, port, err := net.SplitHostPort(addr)
	if err != nil {
		return addr
	}
	if host == "" {
		host = "127.0.0.1"
	}
	return net.JoinHostPort(host, port)
}

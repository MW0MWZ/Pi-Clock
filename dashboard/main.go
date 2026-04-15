/*
 * main.go - Pi-Clock Dashboard
 *
 * Single-binary web dashboard for managing the Pi-Clock display.
 * Embeds all HTML, CSS, and JS assets. Authenticates against the
 * system pi-clock user via /etc/shadow. Serves HTTPS on port 443 with
 * auto-generated self-signed certificate. HTTP on port 80 redirects
 * to HTTPS.
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

package main

import (
	"fmt"
	"log"
	"net/http"
	"os"

	"github.com/MW0MWZ/Pi-Clock/dashboard/internal/api"
	"github.com/MW0MWZ/Pi-Clock/dashboard/internal/auth"
	"github.com/MW0MWZ/Pi-Clock/dashboard/internal/config"
	"github.com/MW0MWZ/Pi-Clock/dashboard/internal/mdns"
	"github.com/go-chi/chi/v5"
	"github.com/go-chi/chi/v5/middleware"
)

// version is set at build time via -ldflags
var version = "dev"

func main() {
	httpsAddr := ":443"
	httpAddr := ":80"

	// Allow override for development
	if os.Getenv("PIC_HTTPS_LISTEN") != "" {
		httpsAddr = os.Getenv("PIC_HTTPS_LISTEN")
	}
	if os.Getenv("PIC_HTTP_LISTEN") != "" {
		httpAddr = os.Getenv("PIC_HTTP_LISTEN")
	}

	// Ensure TLS certificate exists (generate self-signed if needed)
	certDir := os.Getenv("PIC_CERT_DIR")
	certPath, keyPath, err := auth.EnsureTLS(certDir)
	if err != nil {
		log.Fatalf("tls: %v", err)
	}

	// Load renderer configuration
	cfg := config.Load()

	// Initialise session store (30 minute expiry)
	sessions := auth.NewSessionStore(1800)

	// Build router with middleware stack:
	//   Logger    — logs every request (method, path, status, duration)
	//   Recoverer — catches panics in handlers, returns 500 instead of crashing
	//   Compress  — gzip responses > threshold (level 5 = balanced speed/ratio)
	r := chi.NewRouter()
	r.Use(middleware.Logger)
	r.Use(middleware.Recoverer)
	r.Use(middleware.Compress(5))
	r.Use(securityHeaders)

	// Static assets (embedded)
	staticFS := staticFileSystem()
	r.Handle("/static/*", http.StripPrefix("/static/",
		http.FileServerFS(staticFS)))

	// Public routes
	r.Get("/", dashboardHandler(cfg, sessions, version))
	r.Get("/login", loginPageHandler(version))
	r.Post("/login", loginRateLimiter(loginSubmitHandler(sessions)))
	r.Post("/logout", logoutHandler(sessions))

	// API routes (require auth, 1MB body limit)
	r.Route("/api", func(r chi.Router) {
		r.Use(auth.RequireAuth(sessions))
		r.Use(bodyLimitMiddleware(1 << 20)) /* 1MB */

		// Display settings
		r.Get("/config", api.GetConfig(cfg))
		r.Put("/config", api.PutConfig(cfg))

		// Layer control
		r.Get("/layers", api.GetLayers())
		r.Put("/layers", api.PutLayers())

		// System info
		r.Get("/system/info", api.GetSystemInfo)
		r.Get("/system/slot", api.GetSlotInfo)

		// System actions
		r.Post("/system/reboot", api.PostReboot)
		r.Post("/system/shutdown", api.PostShutdown)
		r.Post("/system/apk-upgrade", api.PostApkUpgrade)
		r.Post("/system/os-upgrade", api.PostOsUpgrade)
		r.Post("/system/rollback", api.PostRollback)

		// DX Cluster settings
		r.Get("/dx-settings", api.GetDxSettings())
		r.Put("/dx-settings", api.PutDxSettings(cfg))

		// Propagation / Band Conditions settings
		r.Get("/prop-settings", api.GetPropSettings())
		r.Put("/prop-settings", api.PutPropSettings(cfg))

		// Lightning settings
		r.Get("/lightning-settings", api.GetLightningSettings())
		r.Put("/lightning-settings", api.PutLightningSettings(cfg))

		// Earthquake settings
		r.Get("/earthquake-settings", api.GetEarthquakeSettings())
		r.Put("/earthquake-settings", api.PutEarthquakeSettings(cfg))

		// Applet settings
		r.Get("/applets", api.GetApplets())
		r.Put("/applets", api.PutApplets())

		// Satellite settings
		r.Get("/satellites", api.GetSatellites())
		r.Put("/satellites", api.PutSatellites())

		// Ticker source settings
		r.Get("/ticker-sources", api.GetTickerSources())
		r.Put("/ticker-sources", api.PutTickerSources())

		// Network
		r.Get("/network/wifi", api.GetWifi)
		r.Put("/network/wifi", api.PutWifi)

		// System settings
		r.Get("/system/hostname", api.GetHostname)
		r.Get("/system/update-check", api.GetUpdateCheck)
		r.Put("/system/hostname", api.PutHostname)
		r.Post("/system/change-password", api.PostChangePassword(sessions))
	})

	fmt.Printf("Pi-Clock Dashboard %s\n", version)

	// Start HTTP → HTTPS redirect on port 80 in a separate goroutine.
	// Runs concurrently with the main HTTPS server below. Uses a
	// goroutine (not a separate process) because both servers share
	// the same binary — if the HTTPS server dies, this exits too.
	go func() {
		fmt.Printf("HTTP redirect on %s\n", httpAddr)
		redirect := http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			/* Redirect HTTP to HTTPS. Uses r.Host so the redirect
			 * works via IP, hostname, or mDNS. Safe for a local
			 * network appliance — not exposed to the internet. */
			target := "https://" + r.Host + r.URL.Path
			if r.URL.RawQuery != "" {
				target += "?" + r.URL.RawQuery
			}
			http.Redirect(w, r, target, http.StatusMovedPermanently)
		})
		if err := http.ListenAndServe(httpAddr, redirect); err != nil {
			log.Printf("http redirect: %v", err)
		}
	}()

	// Start mDNS responder — only on Pi-Clock OS (not dev machines)
	if api.IsPiClockOS() {
		mdnsResp := mdns.Start([]mdns.ServicePort{
			mdns.FormatService("https", httpsAddr),
			mdns.FormatService("http", httpAddr),
		})
		defer mdnsResp.Close()
	}

	// Start HTTPS server
	fmt.Printf("HTTPS on %s\n", httpsAddr)
	if err := http.ListenAndServeTLS(httpsAddr, certPath, keyPath, r); err != nil {
		log.Fatalf("https: %v", err)
	}
}

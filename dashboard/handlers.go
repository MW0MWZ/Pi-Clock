/*
 * handlers.go - HTTP handlers for page routes
 *
 * Handles the dashboard page, login page, login/logout actions.
 * Uses Go html/template with embedded template files.
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

package main

import (
	"html/template"
	"log"
	"net"
	"net/http"
	"sync"
	"time"

	"github.com/MW0MWZ/Pi-Clock/dashboard/internal/auth"
	"github.com/MW0MWZ/Pi-Clock/dashboard/internal/config"
)

// templateData holds data passed to HTML templates.
type templateData struct {
	Version string
	Config  *config.RendererConfig
	Error   string
}

// loadTemplate parses a template from the embedded filesystem.
// Panics on error rather than returning it — templates are compiled-in
// assets, so a parse failure means a build defect, not a runtime
// condition the caller can recover from.
func loadTemplate(name string) *template.Template {
	tmpl, err := template.ParseFS(content, "web/templates/"+name)
	if err != nil {
		panic("template: " + err.Error())
	}
	return tmpl
}

// dashboardHandler serves the main dashboard page.
// If not logged in, redirects to login.
//
// Re-reads the renderer config on every page load so the dashboard
// always reflects the current state — the renderer may have been
// reconfigured externally (e.g., by pi-clock-config.txt at boot).
func dashboardHandler(cfg *config.RendererConfig, sessions *auth.SessionStore, ver string) http.HandlerFunc {
	tmpl := loadTemplate("dashboard.html")

	return func(w http.ResponseWriter, r *http.Request) {
		if !sessions.IsAuthenticated(r) {
			http.Redirect(w, r, "/login", http.StatusSeeOther)
			return
		}

		cfg.Reload()
		snap := cfg.Snapshot()

		if err := tmpl.Execute(w, templateData{
			Version: ver,
			Config:  &snap,
		}); err != nil {
			log.Printf("template dashboard: %v", err)
		}
	}
}

// loginPageHandler serves the login form.
func loginPageHandler(ver string) http.HandlerFunc {
	tmpl := loadTemplate("login.html")

	return func(w http.ResponseWriter, r *http.Request) {
		if err := tmpl.Execute(w, templateData{Version: ver}); err != nil {
			log.Printf("template login: %v", err)
		}
	}
}

// loginSubmitHandler processes login form submissions.
func loginSubmitHandler(sessions *auth.SessionStore) http.HandlerFunc {
	tmpl := loadTemplate("login.html")

	return func(w http.ResponseWriter, r *http.Request) {
		/* Username is always "pi-clock" — single-user appliance.
		 * Never accept the username from form input. */
		password := r.FormValue("password")

		/* Reject excessively long passwords before hashing — a multi-MB
		 * string would DoS the Pi Zero W's CPU via openssl passwd. */
		if len(password) > 128 {
			tmpl.Execute(w, templateData{Error: "Invalid password"})
			return
		}

		if !auth.VerifyPassword("pi-clock", password) {
			log.Printf("login: failed attempt from %s", r.RemoteAddr)
			if err := tmpl.Execute(w, templateData{Error: "Invalid password"}); err != nil {
				log.Printf("template login (failed auth): %v", err)
			}
			return
		}

		sessions.Create(w)
		http.Redirect(w, r, "/", http.StatusSeeOther)
	}
}

// logoutHandler destroys the session and redirects to login.
func logoutHandler(sessions *auth.SessionStore) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		sessions.Destroy(w, r)
		http.Redirect(w, r, "/login", http.StatusSeeOther)
	}
}

// loginRateLimiter wraps a handler with per-IP rate limiting.
// Allows 5 attempts per minute to prevent brute-force attacks while
// still being forgiving of mistyped passwords.
func loginRateLimiter(next http.HandlerFunc) http.HandlerFunc {
	var mu sync.Mutex
	attempts := make(map[string][]time.Time)

	return func(w http.ResponseWriter, r *http.Request) {
		ip, _, err := net.SplitHostPort(r.RemoteAddr)
		if err != nil {
			ip = r.RemoteAddr
		}

		mu.Lock()
		now := time.Now()

		// Prevent unbounded map growth — evict one random entry
		// rather than clearing all (which resets active rate limits)
		if len(attempts) > 1000 {
			for k := range attempts {
				delete(attempts, k)
				break
			}
		}

		// Remove attempts older than 1 minute
		recent := attempts[ip][:0]
		for _, t := range attempts[ip] {
			if now.Sub(t) < time.Minute {
				recent = append(recent, t)
			}
		}
		attempts[ip] = recent

		if len(recent) >= 5 {
			mu.Unlock()
			log.Printf("login: rate limited %s (%d attempts/min)", ip, len(recent))
			http.Error(w, "Too many login attempts. Try again in a minute.", http.StatusTooManyRequests)
			return
		}
		attempts[ip] = append(attempts[ip], now)
		mu.Unlock()

		next.ServeHTTP(w, r)
	}
}

// bodyLimitMiddleware rejects request bodies larger than maxBytes.
// Protects the Pi Zero W (512MB RAM) from memory exhaustion via
// oversized POST payloads.
func bodyLimitMiddleware(maxBytes int64) func(http.Handler) http.Handler {
	return func(next http.Handler) http.Handler {
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			r.Body = http.MaxBytesReader(w, r.Body, maxBytes)
			next.ServeHTTP(w, r)
		})
	}
}

// securityHeaders adds standard security headers to all responses.
func securityHeaders(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("X-Content-Type-Options", "nosniff")
		w.Header().Set("X-Frame-Options", "DENY")
		w.Header().Set("Strict-Transport-Security", "max-age=31536000")
		w.Header().Set("Content-Security-Policy",
			"default-src 'self'; style-src 'self' 'unsafe-inline'; script-src 'self' 'unsafe-inline'")
		next.ServeHTTP(w, r)
	})
}

/*
 * session.go - In-memory session management
 *
 * Maintains authenticated sessions with secure cookies.
 * Sessions expire after a configurable timeout (default 30 min).
 * A background goroutine cleans expired sessions every 5 minutes.
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

package auth

import (
	"crypto/rand"
	"encoding/hex"
	"io"
	"net/http"
	"sync"
	"time"
)

const cookieName = "pic_session"

// session represents an authenticated session.
// Single-user appliance — no need to track username.
type session struct {
	expiresAt time.Time
}

// SessionStore manages authenticated sessions in memory.
type SessionStore struct {
	mu       sync.RWMutex
	sessions map[string]*session
	maxAge   time.Duration
}

// NewSessionStore creates a session store with the given max age in seconds.
func NewSessionStore(maxAgeSeconds int) *SessionStore {
	s := &SessionStore{
		sessions: make(map[string]*session),
		maxAge:   time.Duration(maxAgeSeconds) * time.Second,
	}

	// Background cleanup of expired sessions
	go func() {
		for {
			time.Sleep(5 * time.Minute)
			s.cleanup()
		}
	}()

	return s
}

// Create starts a new session and sets a cookie.
func (s *SessionStore) Create(w http.ResponseWriter) {
	token := generateToken()

	s.mu.Lock()
	s.sessions[token] = &session{
		expiresAt: time.Now().Add(s.maxAge),
	}
	s.mu.Unlock()

	// HttpOnly prevents JavaScript access (XSS defence).
	// Secure ensures the cookie is only sent over HTTPS.
	// SameSite=Strict prevents cross-site request forgery — the cookie
	// is never sent on cross-origin requests, eliminating CSRF without
	// needing a separate token. Appropriate for an appliance dashboard
	// that is never embedded in another site.
	http.SetCookie(w, &http.Cookie{
		Name:     cookieName,
		Value:    token,
		Path:     "/",
		HttpOnly: true,
		Secure:   true,
		SameSite: http.SameSiteStrictMode,
		MaxAge:   int(s.maxAge.Seconds()),
	})
}

// IsAuthenticated checks if the request has a valid session.
func (s *SessionStore) IsAuthenticated(r *http.Request) bool {
	cookie, err := r.Cookie(cookieName)
	if err != nil {
		return false
	}

	s.mu.RLock()
	sess, ok := s.sessions[cookie.Value]
	s.mu.RUnlock()

	if !ok || time.Now().After(sess.expiresAt) {
		return false
	}

	return true
}

// DestroyAll removes all sessions — used on password change to
// invalidate any other active sessions (e.g., a hijacked token).
func (s *SessionStore) DestroyAll() {
	s.mu.Lock()
	s.sessions = make(map[string]*session)
	s.mu.Unlock()
}

// Destroy removes the session and clears the cookie.
func (s *SessionStore) Destroy(w http.ResponseWriter, r *http.Request) {
	cookie, err := r.Cookie(cookieName)
	if err == nil {
		s.mu.Lock()
		delete(s.sessions, cookie.Value)
		s.mu.Unlock()
	}

	http.SetCookie(w, &http.Cookie{
		Name:     cookieName,
		Value:    "",
		Path:     "/",
		HttpOnly: true,
		Secure:   true,
		SameSite: http.SameSiteStrictMode,
		MaxAge:   -1,
	})
}

// RequireAuth is middleware that rejects unauthenticated API requests.
func RequireAuth(sessions *SessionStore) func(http.Handler) http.Handler {
	return func(next http.Handler) http.Handler {
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			if !sessions.IsAuthenticated(r) {
				http.Error(w, `{"error":"unauthorized"}`, http.StatusUnauthorized)
				return
			}
			next.ServeHTTP(w, r)
		})
	}
}

// cleanup removes expired sessions.
func (s *SessionStore) cleanup() {
	s.mu.Lock()
	defer s.mu.Unlock()

	now := time.Now()
	for token, sess := range s.sessions {
		if now.After(sess.expiresAt) {
			delete(s.sessions, token)
		}
	}
}

// generateToken creates a cryptographically random session token.
func generateToken() string {
	b := make([]byte, 32)
	if _, err := io.ReadFull(rand.Reader, b); err != nil {
		panic("crypto/rand: " + err.Error())
	}
	return hex.EncodeToString(b)
}

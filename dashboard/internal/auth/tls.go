/*
 * tls.go - Auto-generated self-signed TLS certificates
 *
 * Generates a self-signed certificate on first run and stores it
 * in /data/etc/pi-clock-certs/. The cert is reused on subsequent boots.
 * Valid for 10 years with the system hostname as the CN/SAN.
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

package auth

import (
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/pem"
	"fmt"
	"math/big"
	"net"
	"os"
	"path/filepath"
	"time"
)

const (
	defaultCertDir  = "/data/etc/pi-clock-certs"
	certFileName    = "pi-clock-dashboard.crt"
	keyFileName     = "pi-clock-dashboard.key"
)

// EnsureTLS checks for existing TLS cert/key files and generates
// self-signed ones if they don't exist. Returns the paths to the
// cert and key files.
func EnsureTLS(certDir string) (certPath, keyPath string, err error) {
	if certDir == "" {
		certDir = defaultCertDir
	}

	certPath = filepath.Join(certDir, certFileName)
	keyPath = filepath.Join(certDir, keyFileName)

	// Check if cert and key already exist
	if fileExists(certPath) && fileExists(keyPath) {
		fmt.Printf("tls: using existing certificate from %s\n", certDir)
		return certPath, keyPath, nil
	}

	// Generate new self-signed certificate
	fmt.Println("tls: generating self-signed certificate...")

	if err := os.MkdirAll(certDir, 0700); err != nil {
		return "", "", fmt.Errorf("tls: mkdir %s: %w", certDir, err)
	}

	// Generate ECDSA P-256 private key (fast, small, secure)
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		return "", "", fmt.Errorf("tls: generate key: %w", err)
	}

	// Get hostname for the certificate CN and SAN
	hostname, _ := os.Hostname()
	if hostname == "" {
		hostname = "pi-clock"
	}

	// Random serial number — RFC 5280 recommends unique serials.
	// Avoids browser caching issues if the cert is ever regenerated.
	serialMax := new(big.Int).Lsh(big.NewInt(1), 128)
	serial, err := rand.Int(rand.Reader, serialMax)
	if err != nil {
		return "", "", fmt.Errorf("tls: generate serial: %w", err)
	}

	// Create certificate template
	template := x509.Certificate{
		SerialNumber: serial,
		Subject: pkix.Name{
			CommonName:   hostname,
			Organization: []string{"Pi-Clock"},
		},
		NotBefore: time.Now(),
		NotAfter:  time.Now().Add(10 * 365 * 24 * time.Hour), // 10 years

		KeyUsage:              x509.KeyUsageDigitalSignature | x509.KeyUsageKeyEncipherment,
		ExtKeyUsage:           []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
		BasicConstraintsValid: true,

		// SAN list covers common access patterns:
		//   hostname       — direct DNS on LAN (e.g. "pi-clock")
		//   hostname.local — mDNS/Bonjour (avahi), common on home networks
		//   localhost       — loopback for local testing
		//   127.0.0.1      — direct IP loopback
		DNSNames:    []string{hostname, hostname + ".local", "localhost"},
		IPAddresses: []net.IP{net.ParseIP("127.0.0.1")},
	}

	// Self-sign the certificate
	certDER, err := x509.CreateCertificate(rand.Reader, &template, &template,
		&key.PublicKey, key)
	if err != nil {
		return "", "", fmt.Errorf("tls: create cert: %w", err)
	}

	// Write certificate PEM
	certFile, err := os.OpenFile(certPath, os.O_WRONLY|os.O_CREATE|os.O_TRUNC, 0644)
	if err != nil {
		return "", "", fmt.Errorf("tls: write cert: %w", err)
	}
	if err := pem.Encode(certFile, &pem.Block{Type: "CERTIFICATE", Bytes: certDER}); err != nil {
		certFile.Close()
		return "", "", fmt.Errorf("tls: encode cert PEM: %w", err)
	}
	if err := certFile.Close(); err != nil {
		return "", "", fmt.Errorf("tls: close cert file: %w", err)
	}

	// Write private key PEM
	keyDER, err := x509.MarshalECPrivateKey(key)
	if err != nil {
		return "", "", fmt.Errorf("tls: marshal key: %w", err)
	}
	keyFile, err := os.OpenFile(keyPath, os.O_WRONLY|os.O_CREATE|os.O_TRUNC, 0600)
	if err != nil {
		return "", "", fmt.Errorf("tls: write key: %w", err)
	}
	if err := pem.Encode(keyFile, &pem.Block{Type: "EC PRIVATE KEY", Bytes: keyDER}); err != nil {
		keyFile.Close()
		return "", "", fmt.Errorf("tls: encode key PEM: %w", err)
	}
	if err := keyFile.Close(); err != nil {
		return "", "", fmt.Errorf("tls: close key file: %w", err)
	}

	fmt.Printf("tls: certificate generated for %s\n", hostname)
	return certPath, keyPath, nil
}

func fileExists(path string) bool {
	_, err := os.Stat(path)
	return err == nil
}

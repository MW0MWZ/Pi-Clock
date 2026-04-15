/*
 * shadow.go - System user authentication via /etc/shadow
 *
 * Verifies passwords against the system shadow file. Requires
 * the process to run as root (or have read access to /etc/shadow).
 *
 * Supports the hash algorithms commonly used on Alpine Linux:
 *   $6$ = SHA-512 (default on Alpine)
 *   $5$ = SHA-256
 *   $1$ = MD5 (legacy)
 *
 * Based on the approach used in Pi-Star_MCP.
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

package auth

import (
	"bufio"
	"crypto/subtle"
	"os"
	"os/exec"
	"strings"
)

// VerifyPassword checks a username/password against /etc/shadow.
// Returns true if the credentials are valid.
func VerifyPassword(username, password string) bool {
	hash, err := GetShadowHash(username)
	if err != nil || hash == "" || hash == "!" || hash == "*" {
		return false
	}

	// Extract the algorithm and salt from the stored hash.
	// Shadow hash format: $algo$salt$hash (e.g. "$6$abc123$longhash...")
	// SplitN with limit 4 yields: ["", algo, salt, hash]
	// (the empty string at index 0 is before the leading $)
	parts := strings.SplitN(hash, "$", 4)
	if len(parts) < 4 {
		return false
	}

	// parts[1] = algorithm ("6"=SHA-512, "5"=SHA-256, "1"=MD5)
	// parts[2] = salt value
	algo := parts[1]
	// Reconstruct in openssl passwd format: "$algo$salt$"
	salt := "$" + algo + "$" + parts[2] + "$"

	// Use openssl to compute the hash with the same salt
	computed, err := computeHash(password, salt, algo)
	if err != nil {
		return false
	}

	// Constant-time comparison to prevent timing attacks
	return subtle.ConstantTimeCompare([]byte(hash), []byte(computed)) == 1
}

// GetShadowHash reads the password hash for a user from /etc/shadow.
func GetShadowHash(username string) (string, error) {
	f, err := os.Open("/etc/shadow")
	if err != nil {
		return "", err
	}
	defer f.Close()

	scanner := bufio.NewScanner(f)
	prefix := username + ":"

	for scanner.Scan() {
		line := scanner.Text()
		if strings.HasPrefix(line, prefix) {
			fields := strings.SplitN(line, ":", 3)
			if len(fields) >= 2 {
				return fields[1], nil
			}
		}
	}

	return "", scanner.Err()
}

// computeHash uses openssl to hash a password with the given salt.
func computeHash(password, salt, algo string) (string, error) {
	var flag string
	switch algo {
	case "6":
		flag = "-6"
	case "5":
		flag = "-5"
	case "1":
		flag = "-1"
	default:
		flag = "-6"
	}

	// Extract the bare salt value from "$algo$saltvalue$" for the
	// openssl -salt flag. Trim outer $ chars → "algo$saltvalue",
	// then split on $ and take the last element → "saltvalue".
	saltParts := strings.Split(strings.Trim(salt, "$"), "$")
	saltValue := saltParts[len(saltParts)-1]

	cmd := exec.Command("openssl", "passwd", flag, "-salt",
		saltValue, "-stdin")
	cmd.Stdin = strings.NewReader(password)

	out, err := cmd.Output()
	if err != nil {
		return "", err
	}

	return strings.TrimSpace(string(out)), nil
}

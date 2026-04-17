/*
 * handlers.go - REST API handlers for the dashboard
 *
 * All API responses are JSON. State-changing endpoints require
 * authentication (enforced by middleware).
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

package api

import (
	"bufio"
	"crypto/pbkdf2"
	"crypto/sha1"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"image"
	"image/color"
	"image/png"
	"io"
	"log"
	"net/http"
	"os"
	"os/exec"
	"regexp"
	"runtime"
	"sync"
	"sync/atomic"
	"strconv"
	"strings"
	"syscall"
	"time"

	"github.com/MW0MWZ/Pi-Clock/dashboard/internal/auth"
	"github.com/MW0MWZ/Pi-Clock/dashboard/internal/config"
)

const userConfPath = "/data/etc/pi-clock-user.conf"

// wpaSupplicantConfPath is the live wpa_supplicant configuration. On
// Pi-Clock OS the parent directory is bind-mounted from /data/etc, so
// writes here persist across A/B root-slot upgrades.
const wpaSupplicantConfPath = "/etc/wpa_supplicant/wpa_supplicant.conf"

// IsPiClockOS returns true if running on Pi-Clock OS (not a dev machine).
func IsPiClockOS() bool {
	_, err := os.Stat("/etc/pi-clock-os-release")
	return err == nil
}

// validName matches config key names: lowercase letters and underscores only.
// Prevents injection of arbitrary keys into config files.
var validName = regexp.MustCompile(`^[a-z][a-z0-9_]*$`)

// validDxValue matches DX setting values: digits, hex chars, or empty.
var validDxValue = regexp.MustCompile(`^[0-9a-fA-F]*$`)

// atomicWriteFile writes data to path via a temp file + rename.
// Prevents corrupt config files on power loss (common on Pi appliances).
func atomicWriteFile(path string, data []byte, perm os.FileMode) error {
	tmp := path + ".tmp"
	if err := os.WriteFile(tmp, data, perm); err != nil {
		return err
	}
	if err := os.Rename(tmp, path); err != nil {
		os.Remove(tmp)
		return err
	}
	return nil
}

// userConfMu serialises read-modify-write operations on pi-clock-user.conf.
// Without this, concurrent PutHostname and PostChangePassword calls could
// each read the file, modify different keys, and overwrite each other.
var userConfMu sync.Mutex

// updateUserConf updates a key=value in pi-clock-user.conf.
// This is the persistent config that survives A/B OS upgrades —
// pi-clock-config reads it on boot to rebuild /etc/shadow, hostname, etc.
func updateUserConf(key, value string) error {
	userConfMu.Lock()
	defer userConfMu.Unlock()
	data, err := os.ReadFile(userConfPath)
	if err != nil {
		return err
	}

	found := false
	lines := strings.Split(string(data), "\n")
	for i, line := range lines {
		if strings.HasPrefix(line, key+"=") {
			lines[i] = key + "=" + value
			found = true
			break
		}
	}
	if !found {
		lines = append(lines, key+"="+value)
	}

	return atomicWriteFile(userConfPath, []byte(strings.Join(lines, "\n")), 0600)
}

// jsonResponse writes a JSON response with the given status code.
func jsonResponse(w http.ResponseWriter, status int, data interface{}) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	json.NewEncoder(w).Encode(data)
}

// internalError logs the real error and returns a generic message.
// Prevents leaking internal file paths and system details to the client.
func internalError(w http.ResponseWriter, context string, err error) {
	log.Printf("%s: %v", context, err)
	jsonResponse(w, http.StatusInternalServerError, map[string]string{
		"error": context + " failed",
	})
}

// GetConfig returns the current renderer configuration.
// Uses Snapshot() to read fields under the lock, then serialises
// the copy — prevents data races with concurrent PutConfig.
func GetConfig(cfg *config.RendererConfig) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		cfg.Reload()
		snap := cfg.Snapshot()
		jsonResponse(w, http.StatusOK, snap)
	}
}

// PutConfig updates the renderer configuration.
func PutConfig(cfg *config.RendererConfig) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		var values map[string]string
		if err := json.NewDecoder(r.Body).Decode(&values); err != nil {
			jsonResponse(w, http.StatusBadRequest, map[string]string{
				"error": "invalid JSON",
			})
			return
		}

		if err := cfg.Update(values); err != nil {
			internalError(w, "save config", err)
			return
		}

		// If display_resolution changed, update config.txt so the
		// Pi's framebuffer uses the new resolution on next reboot.
		// The legacy bcm2708_fb driver reads these at boot time.
		if res, ok := values["display_resolution"]; ok && IsPiClockOS() {
			updateConfigTxtResolution(res)
			updateCmdlineVideo(res)
		}

		// Signal the renderer to reload config (SIGHUP)
		signalRenderer()

		jsonResponse(w, http.StatusOK, map[string]string{
			"status": "saved",
		})
	}
}

// layerDefs is the canonical list of display layers. GetLayers merges these
// defaults with the persisted state in pi-clock-layers.conf, so new layers added
// here automatically appear in the dashboard UI with their default values
// even before the user has saved a custom config.
var layerDefs = []struct {
	Name    string
	Label   string
	Default bool
	Opacity float64
}{
	/* UI order: Day Map listed first for readability — the renderer
	 * stack order (basemap painted before daylight) is unaffected
	 * since that is hard-coded in display.c/main.c, not driven by
	 * this list. Labels match the dashboard copy convention. */
	{"daylight", "Day Map", true, 1.0},
	{"basemap", "Night Map", true, 1.0},
	{"borders", "Country Borders", true, 1.0},
	{"grid", "Lat/Lon Grid", false, 1.0},
	{"timezone", "Time Zones", true, 1.0},
	{"cqzone", "CQ Zones", false, 1.0},
	{"ituzone", "ITU Zones", false, 1.0},
	{"maidenhead", "Maidenhead Grid", false, 1.0},
	{"bandconditions", "Propagation Prediction", false, 1.0},
	{"dxspots", "DX Cluster Spots", false, 1.0},
	{"satellites", "Satellite Tracker", false, 1.0},
	{"lightning", "Lightning Strikes", false, 1.0},
	{"earthquakes", "Earthquakes", false, 1.0},
	{"precip", "Rain Radar", false, 1.0},
	{"cloud", "Cloud Cover", false, 1.0},
	{"wind", "Wind", false, 1.0},
	{"aurora", "Aurora", false, 1.0},
	{"qth", "Home QTH Marker", true, 1.0},
	{"sun", "Sun Position", false, 1.0},
	{"moon", "Moon Position", false, 1.0},
	{"ticker", "News Ticker", false, 1.0},
	{"hud", "Clock / HUD", true, 1.0},
}

// layersConfPath is on the persistent /data partition so it survives OS
// upgrades. The renderer checks for changes via the pi-clock-reload trigger file.
const layersConfPath = "/data/etc/pi-clock-layers.conf"

// GetLayers returns current layer settings, merged with defaults.
func GetLayers() http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		// Read current config
		saved := readLayersConf()

		var layers []map[string]interface{}
		for _, def := range layerDefs {
			enabled := def.Default
			opacity := def.Opacity

			if s, ok := saved[def.Name]; ok {
				enabled = s.enabled
				opacity = s.opacity
			}

			layers = append(layers, map[string]interface{}{
				"name":    def.Name,
				"label":   def.Label,
				"enabled": enabled,
				"opacity": opacity,
			})
		}

		jsonResponse(w, http.StatusOK, layers)
	}
}

// PutLayers saves layer settings and signals the renderer.
func PutLayers() http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		var layers []struct {
			Name    string  `json:"name"`
			Enabled bool    `json:"enabled"`
			Opacity float64 `json:"opacity"`
		}

		if err := json.NewDecoder(r.Body).Decode(&layers); err != nil {
			jsonResponse(w, http.StatusBadRequest, map[string]string{
				"error": "invalid JSON",
			})
			return
		}

		// Write the layers config file
		var lines []string
		lines = append(lines, "# Pi-Clock layer configuration")
		lines = append(lines, "# Managed by the dashboard")
		for _, l := range layers {
			if !validName.MatchString(l.Name) {
				continue // skip invalid names
			}
			e := "0"
			if l.Enabled {
				e = "1"
			}
			lines = append(lines, fmt.Sprintf("%s=%s,%.2f", l.Name, e, l.Opacity))
		}

		content := strings.Join(lines, "\n") + "\n"
		if err := atomicWriteFile(layersConfPath, []byte(content), 0644); err != nil {
			internalError(w, "save layers", err)
			return
		}

		// Signal renderer to reload
		signalRenderer()

		jsonResponse(w, http.StatusOK, map[string]string{
			"status": "saved",
		})
	}
}

type layerSetting struct {
	enabled bool
	opacity float64
}

func readLayersConf() map[string]layerSetting {
	result := make(map[string]layerSetting)

	data, err := os.ReadFile(layersConfPath)
	if err != nil {
		return result
	}

	for _, line := range strings.Split(string(data), "\n") {
		line = strings.TrimSpace(line)
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}

		parts := strings.SplitN(line, "=", 2)
		if len(parts) != 2 {
			continue
		}

		name := parts[0]
		if !validName.MatchString(name) {
			continue
		}
		vals := strings.SplitN(parts[1], ",", 2)
		if len(vals) != 2 {
			continue
		}

		enabled := vals[0] == "1"
		opacity := 1.0
		fmt.Sscanf(vals[1], "%f", &opacity)

		result[name] = layerSetting{enabled: enabled, opacity: opacity}
	}

	return result
}

// signalRenderer notifies the renderer that configuration has changed.
// Rather than sending SIGHUP (which requires knowing the PID and risks
// killing the wrong process), we write a trigger file. The renderer
// checks for /tmp/pi-clock-reload each frame, reloads all config files when
// it finds it, then deletes the trigger.
//
// Uses O_NOFOLLOW to prevent symlink attacks on /tmp.
func signalRenderer() {
	f, err := os.OpenFile("/tmp/pi-clock-reload",
		os.O_WRONLY|os.O_CREATE|os.O_TRUNC|syscall.O_NOFOLLOW, 0644)
	if err != nil {
		log.Printf("signalRenderer: %v", err)
		return
	}
	if _, err := f.Write([]byte("1")); err != nil {
		log.Printf("signalRenderer: write: %v", err)
	}
	if err := f.Close(); err != nil {
		log.Printf("signalRenderer: close: %v", err)
	}
}

// updateConfigTxtResolution writes framebuffer_width/height to config.txt
// so the Pi boots at the requested resolution. The legacy bcm2708_fb driver
// reads these values during early boot — runtime changes aren't possible.
func updateConfigTxtResolution(res string) {
	configTxt := "/boot/firmware/config.txt"

	resMap := map[string][2]string{
		"720p":  {"1280", "720"},
		"1080p": {"1920", "1080"},
		"1440p": {"2560", "1440"},
		"4k":    {"3840", "2160"},
	}

	dims, ok := resMap[res]
	if !ok {
		return // Unknown resolution — don't touch config.txt
	}

	data, err := os.ReadFile(configTxt)
	if err != nil {
		log.Printf("updateConfigTxtResolution: can't read %s: %v", configTxt, err)
		return
	}

	content := string(data)
	lines := strings.Split(content, "\n")
	foundW, foundH := false, false

	for i, line := range lines {
		if strings.HasPrefix(line, "framebuffer_width=") {
			lines[i] = "framebuffer_width=" + dims[0]
			foundW = true
		}
		if strings.HasPrefix(line, "framebuffer_height=") {
			lines[i] = "framebuffer_height=" + dims[1]
			foundH = true
		}
	}

	if !foundW {
		lines = append(lines, "framebuffer_width="+dims[0])
	}
	if !foundH {
		lines = append(lines, "framebuffer_height="+dims[1])
	}

	if err := os.WriteFile(configTxt, []byte(strings.Join(lines, "\n")), 0644); err != nil {
		log.Printf("updateConfigTxtResolution: can't write %s: %v", configTxt, err)
	} else {
		log.Printf("config.txt updated: %sx%s for %s", dims[0], dims[1], res)
	}
}

// updateCmdlineVideo writes the video= parameter to cmdline.txt so
// the DRM framebuffer emulation uses 32bpp at the correct resolution.
// This is needed on Pi 5 where KMS is active — without it, the DRM
// fbdev defaults to 16bpp. Harmless on non-KMS models (ignored).
func updateCmdlineVideo(res string) {
	cmdline := "/boot/firmware/cmdline.txt"

	videoMap := map[string]string{
		"720p":  "video=HDMI-A-1:1280x720-32@60",
		"1080p": "video=HDMI-A-1:1920x1080-32@60",
		"1440p": "video=HDMI-A-1:2560x1440-32@60",
		"4k":    "video=HDMI-A-1:3840x2160-32@30",
	}

	videoParam, ok := videoMap[res]
	if !ok {
		return
	}

	data, err := os.ReadFile(cmdline)
	if err != nil {
		log.Printf("updateCmdlineVideo: can't read %s: %v", cmdline, err)
		return
	}

	line := strings.TrimSpace(string(data))

	// Remove any existing video= parameter
	parts := strings.Fields(line)
	var filtered []string
	for _, p := range parts {
		if !strings.HasPrefix(p, "video=") {
			filtered = append(filtered, p)
		}
	}
	filtered = append(filtered, videoParam)

	if err := os.WriteFile(cmdline, []byte(strings.Join(filtered, " ")+"\n"), 0644); err != nil {
		log.Printf("updateCmdlineVideo: can't write %s: %v", cmdline, err)
	} else {
		log.Printf("cmdline.txt updated: %s", videoParam)
	}
}

// GetSystemInfo returns system information (hostname, uptime, etc).
// Includes display status from the init script's status file.
func GetSystemInfo(w http.ResponseWriter, r *http.Request) {
	hostname, _ := os.Hostname()

	uptimeStr := "-"
	if data, err := os.ReadFile("/proc/uptime"); err == nil {
		if fields := strings.Fields(string(data)); len(fields) > 0 {
			uptimeStr = fields[0]
		}
	}

	release, _ := os.ReadFile("/etc/pi-clock-os-release")

	/* Extract version from os-release (line: "Version: 2026-04-13r0") */
	osVersion := ""
	for _, line := range strings.Split(string(release), "\n") {
		line = strings.TrimSpace(line)
		if strings.HasPrefix(line, "Version:") {
			osVersion = strings.TrimSpace(strings.TrimPrefix(line, "Version:"))
		}
	}

	fbSize, _ := os.ReadFile("/sys/class/graphics/fb0/virtual_size")

	/* Parse display status written by the renderer at startup.
	 * Contains the actual resolution, refresh rate, and CPU cores. */
	displayRes := strings.TrimSpace(string(fbSize))
	refreshRate := ""
	cpuCores := ""
	ramTotal := ""
	ramBudget := ""
	if data, err := os.ReadFile("/tmp/pi-clock-cache/display-status"); err == nil {
		for _, line := range strings.Split(string(data), "\n") {
			parts := strings.SplitN(line, "=", 2)
			if len(parts) != 2 {
				continue
			}
			switch parts[0] {
			case "ACTUAL_REFRESH":
				refreshRate = parts[1] + " Hz"
			case "CPU_CORES":
				cpuCores = parts[1]
			case "RAM_TOTAL":
				ramTotal = parts[1]
			case "RAM_BUDGET":
				ramBudget = parts[1]
			}
		}
	}

	isPiClockOS := "false"
	if IsPiClockOS() {
		isPiClockOS = "true"
	}

	/* Detect Pi hardware model from device tree.
	 * Used to limit resolution options — Pi 0/1/2/Zero2W cap at 1080p
	 * (512MB RAM can't hold 4K layer surfaces), Pi 3+ allows up to 4K. */
	piModel := ""
	maxRes := "4k"
	if data, err := os.ReadFile("/proc/device-tree/model"); err == nil {
		piModel = strings.TrimRight(string(data), "\x00\n")
		/* Only Pi 4/5 (and their Compute Modules) have HDMI 2.0 capable
		 * of 4K output. Everything else — Pi 0/1/2/3/Zero2W — is hardware
		 * limited to 1080p max over HDMI 1.4. */
		is4kCapable := false
		for _, prefix := range []string{"Raspberry Pi 4", "Raspberry Pi 5", "Raspberry Pi Compute Module 4", "Raspberry Pi Compute Module 5"} {
			if strings.HasPrefix(piModel, prefix) {
				is4kCapable = true
				break
			}
		}
		if !is4kCapable {
			maxRes = "1080p"
		}
	}

	/* Check if a reboot is required. We build a list of reasons
	 * so the dashboard can show a persistent banner. Reasons:
	 *   - Resolution change pending (config != actual)
	 *   - OS upgrade pending (PENDING=true in slot.conf)
	 *   - Package upgrade pending (marker file exists) */
	var rebootReasons []string

	/* Resolution mismatch: compare configured vs actual */
	if data, err := os.ReadFile("/data/etc/pi-clock-renderer.conf"); err == nil {
		resMap := map[string]string{
			"720p": "1280,720", "1080p": "1920,1080",
			"1440p": "2560,1440", "4k": "3840,2160",
		}
		for _, line := range strings.Split(string(data), "\n") {
			parts := strings.SplitN(line, "=", 2)
			if len(parts) == 2 && parts[0] == "DISPLAY_RESOLUTION" {
				configured := strings.TrimSpace(parts[1])
				expected := resMap[configured]
				if expected != "" && displayRes != "" && expected != displayRes {
					rebootReasons = append(rebootReasons,
						"Resolution change pending ("+configured+")")
				}
				break
			}
		}
	}

	/* OS slot upgrade pending */
	if data, err := os.ReadFile("/boot/firmware/slot.conf"); err == nil {
		for _, line := range strings.Split(string(data), "\n") {
			if strings.TrimSpace(line) == "PENDING=true" {
				rebootReasons = append(rebootReasons,
					"OS upgrade pending")
				break
			}
		}
	}

	/* APK upgrade pending — marker file created by apk-upgrade handler */
	if _, err := os.Stat("/tmp/pi-clock-apk-upgraded"); err == nil {
		rebootReasons = append(rebootReasons,
			"Package upgrade pending")
	}

	rebootRequired := "false"
	rebootReasonStr := ""
	if len(rebootReasons) > 0 {
		rebootRequired = "true"
		rebootReasonStr = strings.Join(rebootReasons, "; ")
	}

	info := map[string]string{
		"hostname":        hostname,
		"uptime":          uptimeStr,
		"arch":            runtime.GOARCH,
		"release":         strings.TrimSpace(string(release)),
		"os_version":      osVersion,
		"display_size":    displayRes,
		"refresh_rate":    refreshRate,
		"cpu_cores":       cpuCores,
		"pi_clock_os":     isPiClockOS,
		"pi_model":        piModel,
		"max_resolution":  maxRes,
		"ram_total":       ramTotal,
		"ram_budget":      ramBudget,
		"reboot_required": rebootRequired,
		"reboot_reasons":  rebootReasonStr,
	}

	jsonResponse(w, http.StatusOK, info)
}

// GetSlotInfo returns the A/B slot status.
func GetSlotInfo(w http.ResponseWriter, r *http.Request) {
	data, err := os.ReadFile("/boot/firmware/slot.conf")
	if err != nil {
		jsonResponse(w, http.StatusInternalServerError, map[string]string{
			"error": "cannot read slot.conf",
		})
		return
	}

	info := make(map[string]string)
	for _, line := range strings.Split(string(data), "\n") {
		parts := strings.SplitN(line, "=", 2)
		if len(parts) == 2 {
			info[strings.TrimSpace(parts[0])] = strings.Trim(strings.TrimSpace(parts[1]), "\"")
		}
	}

	jsonResponse(w, http.StatusOK, info)
}

// GetScreenshot reads the Linux framebuffer (/dev/fb0) and returns
// it as a PNG image. The framebuffer pixel format on the Pi is BGRA
// (32-bit), which we convert to RGBA for the PNG encoder.
//
// Resolution is read from /sys/class/graphics/fb0/virtual_size and
// stride from /sys/class/graphics/fb0/stride. This works on all Pi
// models regardless of resolution.
func GetScreenshot(w http.ResponseWriter, r *http.Request) {
	/* Read framebuffer dimensions */
	sizeData, err := os.ReadFile("/sys/class/graphics/fb0/virtual_size")
	if err != nil {
		http.Error(w, "cannot read framebuffer size", http.StatusInternalServerError)
		return
	}
	dims := strings.Split(strings.TrimSpace(string(sizeData)), ",")
	if len(dims) != 2 {
		http.Error(w, "unexpected framebuffer size format", http.StatusInternalServerError)
		return
	}
	width, _ := strconv.Atoi(dims[0])
	height, _ := strconv.Atoi(dims[1])
	if width <= 0 || height <= 0 {
		http.Error(w, "invalid framebuffer dimensions", http.StatusInternalServerError)
		return
	}

	/* Read stride (bytes per row — may be padded beyond width*4) */
	stride := width * 4
	if strideData, err := os.ReadFile("/sys/class/graphics/fb0/stride"); err == nil {
		if s, err := strconv.Atoi(strings.TrimSpace(string(strideData))); err == nil && s > 0 {
			stride = s
		}
	}

	/* Sanity checks — prevent panics from corrupt sysfs values */
	if width > 7680 || height > 4320 || stride < width*4 {
		http.Error(w, "framebuffer dimensions out of range", http.StatusInternalServerError)
		return
	}

	/* Read the raw framebuffer */
	fb, err := os.Open("/dev/fb0")
	if err != nil {
		http.Error(w, "cannot open framebuffer", http.StatusInternalServerError)
		return
	}
	defer fb.Close()

	raw := make([]byte, stride*height)
	if _, err := io.ReadFull(fb, raw); err != nil {
		http.Error(w, "cannot read framebuffer", http.StatusInternalServerError)
		return
	}

	/* Convert BGRA framebuffer to RGBA PNG.
	 * The Pi's framebuffer is BGRA in memory (little-endian ARGB32),
	 * so we swap B and R channels for each pixel. */
	img := image.NewNRGBA(image.Rect(0, 0, width, height))
	for y := 0; y < height; y++ {
		rowOff := y * stride
		for x := 0; x < width; x++ {
			off := rowOff + x*4
			b := raw[off+0]
			g := raw[off+1]
			r := raw[off+2]
			/* Alpha channel in framebuffer is often 0 (opaque in
			 * pre-multiplied) — force to 255 for the PNG. */
			img.SetNRGBA(x, y, color.NRGBA{R: r, G: g, B: b, A: 255})
		}
	}

	/* Generate filename with timestamp */
	filename := fmt.Sprintf("pi-clock-%s.png",
		time.Now().UTC().Format("2006-01-02-150405"))

	w.Header().Set("Content-Type", "image/png")
	w.Header().Set("Content-Disposition",
		fmt.Sprintf("attachment; filename=\"%s\"", filename))
	if err := png.Encode(w, img); err != nil {
		log.Printf("GetScreenshot: png encode: %v", err)
	}
}

// PostReboot triggers a system reboot. The response is sent before the
// command runs so the HTTP client receives confirmation before the
// connection is terminated. The goroutine detaches the reboot from
// the request lifecycle.
func PostReboot(w http.ResponseWriter, r *http.Request) {
	jsonResponse(w, http.StatusOK, map[string]string{"status": "rebooting"})
	go exec.Command("reboot").Run()
}

// PostShutdown triggers a system shutdown. Same goroutine pattern as
// PostReboot — respond first, then execute.
func PostShutdown(w http.ResponseWriter, r *http.Request) {
	jsonResponse(w, http.StatusOK, map[string]string{"status": "shutting down"})
	go exec.Command("poweroff").Run()
}

// PostApkUpgrade runs apk update + apk upgrade to update packages.
// Alpine requires a separate "apk update" to refresh the repository
// index before "apk upgrade" can find newer versions. We run them
// sequentially and combine output. Errors from "apk update" are
// intentionally ignored — a stale index may still allow upgrade.
func PostApkUpgrade(w http.ResponseWriter, r *http.Request) {
	updateOut, _ := exec.Command("apk", "update").CombinedOutput()

	// Then apk upgrade
	upgradeOut, err := exec.Command("apk", "upgrade").CombinedOutput()
	combined := string(updateOut) + string(upgradeOut)

	if err != nil {
		jsonResponse(w, http.StatusInternalServerError, map[string]string{
			"error":  "package upgrade failed",
			"output": combined,
		})
		return
	}
	/* Create a marker file so the dashboard shows a reboot banner.
	 * The new binary is installed but not running until reboot.
	 * Deleted automatically on reboot (tmpfs). */
	os.WriteFile("/tmp/pi-clock-apk-upgraded", []byte("1"), 0644)

	jsonResponse(w, http.StatusOK, map[string]string{
		"status": "packages updated",
		"output": combined,
	})
}

// upgradeRunning prevents concurrent OS upgrades — two simultaneous
// upgrade processes would corrupt the inactive partition.
var upgradeRunning int32

// PostOsUpgrade triggers a full OS image upgrade (A/B slot).
// The response is streamed as Server-Sent Events so the dashboard
// can show live progress through each stage of the upgrade.
func PostOsUpgrade(w http.ResponseWriter, r *http.Request) {
	if !atomic.CompareAndSwapInt32(&upgradeRunning, 0, 1) {
		jsonResponse(w, http.StatusConflict, map[string]string{
			"error": "upgrade already in progress",
		})
		return
	}
	defer atomic.StoreInt32(&upgradeRunning, 0)
	flusher, ok := w.(http.Flusher)
	if !ok {
		jsonResponse(w, http.StatusInternalServerError, map[string]string{
			"error": "streaming not supported",
		})
		return
	}

	w.Header().Set("Content-Type", "text/event-stream")
	w.Header().Set("Cache-Control", "no-cache")
	w.Header().Set("X-Accel-Buffering", "no")

	sendEvent := func(stage, message string) {
		payload, _ := json.Marshal(map[string]string{
			"stage": stage, "message": message,
		})
		fmt.Fprintf(w, "data: %s\n\n", payload)
		flusher.Flush()
	}

	cmd := exec.Command("/usr/local/bin/pi-clock-upgrade", "--install")

	// Merge stdout and stderr into a single pipe so we can read
	// every line the upgrade script produces, including wget progress.
	pr, pw := io.Pipe()
	defer pr.Close()
	cmd.Stdout = pw
	cmd.Stderr = pw

	if err := cmd.Start(); err != nil {
		pw.Close()
		log.Printf("os-upgrade: start: %v", err)
		sendEvent("error", "upgrade failed to start")
		return
	}

	// Wait for the command in a goroutine. Closing pw signals
	// EOF to the scanner so the read loop below terminates.
	done := make(chan error, 1)
	go func() {
		done <- cmd.Wait()
		pw.Close()
	}()

	scanner := bufio.NewScanner(pr)
	for scanner.Scan() {
		line := scanner.Text()
		if strings.HasPrefix(line, "STAGE:") {
			sendEvent(strings.TrimPrefix(line, "STAGE:"), "")
		}
	}
	if err := scanner.Err(); err != nil {
		log.Printf("os-upgrade: scanner: %v", err)
	}

	if err := <-done; err != nil {
		sendEvent("error", "upgrade failed")
	}
}

// PostRollback triggers a rollback to the previous slot.
func PostRollback(w http.ResponseWriter, r *http.Request) {
	out, err := exec.Command("/usr/local/bin/pi-clock-rollback").CombinedOutput()
	if err != nil {
		log.Printf("rollback: %v: %s", err, string(out))
		jsonResponse(w, http.StatusInternalServerError, map[string]string{
			"error": "rollback failed",
		})
		return
	}
	jsonResponse(w, http.StatusOK, map[string]string{
		"status": "rollback complete",
		"output": string(out),
	})
}

// GetUpdateCheck checks if a newer OS version is available by
// fetching latest.json from the gh-pages APK repository.
func GetUpdateCheck(w http.ResponseWriter, r *http.Request) {
	/* Read installed version */
	release, _ := os.ReadFile("/etc/pi-clock-os-release")
	installed := ""
	for _, line := range strings.Split(string(release), "\n") {
		line = strings.TrimSpace(line)
		if strings.HasPrefix(line, "Version:") {
			installed = strings.TrimSpace(strings.TrimPrefix(line, "Version:"))
		}
	}

	/* Fetch latest.json from gh-pages using net/http instead of
	 * spawning wget — cheaper on the Pi Zero W and supports
	 * context cancellation if the client disconnects. */
	result := map[string]interface{}{
		"installed": installed,
		"available": "",
		"update":    false,
		"url":       "",
	}

	client := &http.Client{Timeout: 10 * time.Second}
	resp, err := client.Get("https://pi-clock.pistar.uk/latest.json")
	if err == nil {
		defer resp.Body.Close()
		body, _ := io.ReadAll(io.LimitReader(resp.Body, 64*1024))
		var latest map[string]string
		if json.Unmarshal(body, &latest) == nil {
			result["available"] = latest["version"]
			result["url"] = latest["url"]
			if latest["version"] != "" && latest["version"] != installed {
				result["update"] = true
			}
		}
	}

	jsonResponse(w, http.StatusOK, result)
}

// GetHostname returns the current system hostname.
func GetHostname(w http.ResponseWriter, r *http.Request) {
	hostname, _ := os.Hostname()
	jsonResponse(w, http.StatusOK, map[string]string{
		"hostname": hostname,
	})
}

// PutHostname changes the system hostname and persists it.
func PutHostname(w http.ResponseWriter, r *http.Request) {
	var req struct {
		Hostname string `json:"hostname"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		jsonResponse(w, http.StatusBadRequest, map[string]string{"error": "invalid JSON"})
		return
	}

	h := strings.TrimSpace(req.Hostname)
	if h == "" {
		jsonResponse(w, http.StatusBadRequest, map[string]string{"error": "hostname is required"})
		return
	}
	/* Only allow valid hostname characters */
	for _, c := range h {
		if !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
			(c >= '0' && c <= '9') || c == '-') {
			jsonResponse(w, http.StatusBadRequest, map[string]string{
				"error": "hostname may only contain letters, digits, and hyphens",
			})
			return
		}
	}
	if len(h) > 63 {
		jsonResponse(w, http.StatusBadRequest, map[string]string{"error": "hostname too long (max 63)"})
		return
	}
	if h[0] == '-' || h[len(h)-1] == '-' {
		jsonResponse(w, http.StatusBadRequest, map[string]string{
			"error": "hostname must not start or end with a hyphen",
		})
		return
	}

	/* Set hostname in the running system */
	if err := os.WriteFile("/etc/hostname", []byte(h+"\n"), 0644); err != nil {
		internalError(w, "save hostname", err)
		return
	}
	if err := exec.Command("hostname", h).Run(); err != nil {
		log.Printf("hostname: syscall failed: %v", err)
	}

	/* Update /etc/hosts */
	if data, err := os.ReadFile("/etc/hosts"); err == nil {
		lines := strings.Split(string(data), "\n")
		for i, line := range lines {
			if strings.Contains(line, "127.0.1.1") {
				lines[i] = "127.0.1.1   " + h
			}
		}
		if err := os.WriteFile("/etc/hosts", []byte(strings.Join(lines, "\n")), 0644); err != nil {
			log.Printf("hostname: failed to update /etc/hosts: %v", err)
		}
	}

	/* Persist to pi-clock-user.conf for survival across OS upgrades */
	if err := updateUserConf("hostname", h); err != nil {
		log.Printf("hostname: failed to persist: %v", err)
	}

	log.Printf("hostname: changed to %q from %s", h, r.RemoteAddr)
	jsonResponse(w, http.StatusOK, map[string]string{"status": "hostname changed"})
}

// GetWifi returns the current WiFi SSID, country code, and whether the
// configured network is open (unsecured). The PSK is deliberately not
// returned — the dashboard only needs it to decide whether to pre-tick
// the "Open network" box and whether it can let the user save without
// re-entering a password.
func GetWifi(w http.ResponseWriter, r *http.Request) {
	resp := map[string]interface{}{
		"ssid": "", "country": "", "open_network": false,
	}
	data, err := os.ReadFile(wpaSupplicantConfPath)
	if err != nil {
		jsonResponse(w, http.StatusOK, resp)
		return
	}

	var (
		ssid, country string
		inNet         bool
		keyMgmtNone   bool
	)
	for _, raw := range strings.Split(string(data), "\n") {
		line := strings.TrimSpace(raw)
		switch {
		case !inNet && strings.HasPrefix(line, "country="):
			country = line[len("country="):]
		case !inNet && line == "network={":
			inNet = true
		case inNet && line == "}":
			inNet = false
		case inNet && strings.HasPrefix(line, "ssid="):
			/* Strip the surrounding double quotes that wpa_supplicant
			 * requires around string-form SSIDs. Non-ASCII SSIDs can
			 * also appear unquoted as hex; those are rare in the
			 * dashboard flow (we always write quoted form) so we
			 * don't attempt to decode them here. */
			ssid = strings.Trim(line[len("ssid="):], "\"")
		case inNet && line == "key_mgmt=NONE":
			keyMgmtNone = true
		}
	}

	resp["ssid"] = ssid
	resp["country"] = country
	resp["open_network"] = keyMgmtNone
	jsonResponse(w, http.StatusOK, resp)
}

// currentWifiAuthBlock returns the indented auth lines of the existing
// network{} block whose ssid matches targetSSID, as they appear in
// wpa_supplicant.conf between the `ssid=` line and the `priority=`
// line (exclusive at both ends). Typical contents:
//
//	"\t#psk=\"plaintext\"\n\tpsk=<hex hash>"   (wpa_passphrase-formatted secured network)
//	"\tkey_mgmt=NONE"                          (open network)
//
// Used by PutWifi so the user can re-save the same SSID (e.g. to change
// the country code) without re-typing their passphrase — we reuse every
// captured line, preserving both the #psk="..." plaintext comment and
// the hashed psk= line that pi-clock-config / wpa_passphrase write.
// Returns ok=false if the target SSID is not present, in which case the
// caller must refuse the save rather than silently create an open
// network.
func currentWifiAuthBlock(confPath, targetSSID string) (block string, ok bool) {
	data, err := os.ReadFile(confPath)
	if err != nil {
		return "", false
	}
	var (
		inNet     bool
		blockSSID string
		lines     []string
		capturing bool
	)
	for _, raw := range strings.Split(string(data), "\n") {
		t := strings.TrimSpace(raw)
		switch {
		case !inNet && t == "network={":
			inNet = true
			blockSSID = ""
			lines = lines[:0]
			capturing = false
		case inNet && t == "}":
			inNet = false
			if blockSSID == targetSSID && len(lines) > 0 {
				return strings.Join(lines, "\n"), true
			}
		case inNet && strings.HasPrefix(t, "ssid="):
			blockSSID = strings.Trim(t[len("ssid="):], "\"")
			capturing = true
		case inNet && strings.HasPrefix(t, "priority="):
			/* priority is rewritten by PutWifi, so stop capturing
			 * here and drop whatever followed it. */
			capturing = false
		case inNet && capturing && t != "":
			/* Re-emit with a leading tab so the output stays
			 * consistent regardless of the source file's
			 * indentation style. */
			lines = append(lines, "\t"+t)
		}
	}
	return "", false
}

// PutWifi updates the WiFi SSID, password, and country.
//
// Three modes, resolved from the request body:
//
//   1. open_network=true:      write key_mgmt=NONE (unsecured network).
//   2. password non-empty:     derive the PSK via PBKDF2-HMAC-SHA1 per
//                              IEEE 802.11i / RFC 8018 — the same hash
//                              `wpa_passphrase(8)` would produce.
//   3. password empty:         reuse the existing auth line from
//                              wpa_supplicant.conf, but only if the
//                              requested SSID matches the one already
//                              stored. Otherwise reject — we have no
//                              credentials to fall back on and silently
//                              dropping the network to open would be a
//                              data-loss footgun.
//
// PBKDF2 is computed in-process rather than by shelling out to
// `wpa_passphrase`. That removes a whole class of failure modes (PATH,
// exec permissions, version quirks, stdin buffering) and keeps the
// plaintext passphrase out of any child process address space.
func PutWifi(w http.ResponseWriter, r *http.Request) {
	var req struct {
		SSID        string `json:"ssid"`
		Password    string `json:"password"`
		Country     string `json:"country"`
		OpenNetwork bool   `json:"open_network"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		jsonResponse(w, http.StatusBadRequest, map[string]string{"error": "invalid JSON"})
		return
	}

	if req.SSID == "" {
		jsonResponse(w, http.StatusBadRequest, map[string]string{"error": "SSID is required"})
		return
	}
	/* Reject characters that could break the config file format or
	 * escape out of the ssid="..." quoting:
	 *   newline/CR    — inject extra wpa_supplicant directives
	 *   double-quote  — terminate the quoted SSID early
	 *   backslash     — wpa_supplicant interprets \n, \t, etc. inside
	 *                   quoted strings
	 *   null byte     — silently truncates strings in C-based tooling
	 * Applied to every field we embed in the generated config. */
	for _, v := range []string{req.SSID, req.Password, req.Country} {
		if strings.ContainsAny(v, "\n\r\"\\\x00") {
			jsonResponse(w, http.StatusBadRequest, map[string]string{"error": "invalid characters"})
			return
		}
	}
	if strings.HasPrefix(req.SSID, "-") {
		jsonResponse(w, http.StatusBadRequest, map[string]string{"error": "SSID cannot start with a hyphen"})
		return
	}
	if len(req.SSID) > 32 {
		jsonResponse(w, http.StatusBadRequest, map[string]string{"error": "SSID too long (max 32)"})
		return
	}

	/* Country code must be exactly 2 uppercase ASCII letters — wpa_supplicant
	 * and the kernel regdb both require this form. Default to GB if the
	 * client omitted the field entirely. */
	if req.Country == "" {
		req.Country = "GB"
	}
	if len(req.Country) != 2 || req.Country[0] < 'A' || req.Country[0] > 'Z' ||
		req.Country[1] < 'A' || req.Country[1] > 'Z' {
		jsonResponse(w, http.StatusBadRequest, map[string]string{"error": "country must be 2 uppercase letters"})
		return
	}

	/* Resolve the auth block for the network{} section. The three
	 * modes are intentionally mutually exclusive: OpenNetwork wins
	 * over Password if a client somehow sends both, so a deliberate
	 * user tick can't be overridden by a stale value in the password
	 * field. The block is a multi-line chunk (already tab-indented)
	 * that will be spliced into the network{} between ssid= and
	 * priority= — matching the format wpa_passphrase / pi-clock-config
	 * produce, so every writer of this file emits byte-compatible
	 * output. */
	var authBlock string
	switch {
	case req.OpenNetwork:
		authBlock = "\tkey_mgmt=NONE"

	case req.Password != "":
		/* WPA2 passphrase length is fixed by the spec at 8..63 printable
		 * ASCII characters. Enforce here so we don't feed an out-of-spec
		 * input into PBKDF2 and then fail to associate with the AP. */
		if len(req.Password) < 8 || len(req.Password) > 63 {
			jsonResponse(w, http.StatusBadRequest, map[string]string{"error": "WiFi password must be 8-63 characters"})
			return
		}
		/* PSK = PBKDF2-HMAC-SHA1(passphrase, ssid, 4096, 32 bytes).
		 * Defined in IEEE 802.11i §8.4.1.1 and RFC 8018; matches
		 * `wpa_passphrase(8)` byte-for-byte. */
		hash, err := pbkdf2.Key(sha1.New, req.Password, []byte(req.SSID), 4096, 32)
		if err != nil {
			internalError(w, "derive wifi psk", err)
			return
		}
		/* Emit wpa_passphrase's exact two-line format:
		 *   #psk="plaintext"     (ignored by wpa_supplicant, kept as a
		 *                         debugging aid — matches what the
		 *                         boot-time pi-clock-config script
		 *                         writes, so the file looks identical
		 *                         whichever path set it up)
		 *   psk=<hex hash>
		 * Security note: this leaves the plaintext passphrase inside
		 * wpa_supplicant.conf. The file is mode 0600 and owned by root;
		 * only root-equivalent access can read it. This is a
		 * deliberate consistency-over-secrecy tradeoff — pi-clock-config
		 * already does the same, and the Pi's threat model treats
		 * filesystem root as fully trusted. The earlier validation
		 * rejected \ and " so the password can be embedded verbatim. */
		authBlock = fmt.Sprintf("\t#psk=\"%s\"\n\tpsk=%s",
			req.Password, hex.EncodeToString(hash))

	default:
		/* Blank password, not an open network. Only legal if the user
		 * is re-saving the same SSID — then we preserve whatever auth
		 * block was already stored (the #psk/psk pair, or key_mgmt=NONE).
		 * Rejects the SSID-change case so we never silently discard
		 * the user's credentials. */
		block, ok := currentWifiAuthBlock(wpaSupplicantConfPath, req.SSID)
		if !ok {
			jsonResponse(w, http.StatusBadRequest, map[string]string{
				"error": `Password required (enter the WiFi password, or tick "Open network" for an unsecured network)`,
			})
			return
		}
		authBlock = block
	}

	/* Assemble the new wpa_supplicant.conf. The header, tab indents,
	 * quoted-SSID form, and trailing priority=1 exactly mirror what
	 * the on-device pi-clock-config script writes when processing the
	 * boot-partition config file — any tool or admin reading the file
	 * sees a single consistent format regardless of whether the
	 * dashboard or the boot processor last touched it. */
	conf := fmt.Sprintf(`ctrl_interface=/var/run/wpa_supplicant
ctrl_interface_group=0
update_config=1
country=%s
network={
	ssid="%s"
%s
	priority=1
}
`, req.Country, req.SSID, authBlock)

	/* atomicWriteFile writes via tmp + rename so a power loss mid-save
	 * leaves the previous config intact rather than truncating it. */
	if err := atomicWriteFile(wpaSupplicantConfPath, []byte(conf), 0600); err != nil {
		internalError(w, "save wifi", err)
		return
	}

	/* Deliberately do NOT reconfigure wpa_supplicant here. The client
	 * calls POST /api/network/wifi/connect a few seconds later so the
	 * UI can show the "saved" state before we bring the radio down
	 * and back up — the re-association can take 10-20 seconds on a
	 * slow AP and the user's browser session may be on the very WiFi
	 * we're about to disconnect. */

	log.Printf("wifi: SSID changed to %q (open=%v)", req.SSID, req.OpenNetwork)
	jsonResponse(w, http.StatusOK, map[string]string{"status": "saved"})
}

// parseWpaStatus extracts the fields from `wpa_cli status` output that
// we report to the dashboard: wpa_state (the association state machine
// position), ssid (the network name once associated), and ip_address
// (once DHCP has completed). Missing fields are returned as "".
func parseWpaStatus(out []byte) (state, ssid, ip string) {
	for _, line := range strings.Split(string(out), "\n") {
		switch {
		case strings.HasPrefix(line, "wpa_state="):
			state = line[len("wpa_state="):]
		case strings.HasPrefix(line, "ssid="):
			ssid = line[len("ssid="):]
		case strings.HasPrefix(line, "ip_address="):
			ip = line[len("ip_address="):]
		}
	}
	return
}

// PostWifiConnect asks wpa_supplicant to reload the configuration
// written by PutWifi, then polls its status machine until association
// completes or the timeout fires.
//
// Split from PutWifi so the dashboard can show the saved-config state
// before the radio disconnects — the association step can take tens of
// seconds on a slow AP, and if the user is editing over the same WiFi
// they're switching networks on, their browser will miss the response
// anyway. The config is already on disk before this runs; a failing
// connect just means the user has to reboot or fix the AP.
func PostWifiConnect(w http.ResponseWriter, r *http.Request) {
	if err := exec.Command("wpa_cli", "-i", "wlan0", "reconfigure").Run(); err != nil {
		log.Printf("wifi: wpa_cli reconfigure: %v", err)
		jsonResponse(w, http.StatusOK, map[string]interface{}{
			"connected": false,
			"state":     "RECONFIGURE_FAILED",
			"error":     "Could not signal wpa_supplicant — config saved, will apply on next boot",
		})
		return
	}

	/* Poll at 500 ms up to 20 s. wpa_supplicant walks through:
	 *   DISCONNECTED → SCANNING → ASSOCIATING → ASSOCIATED →
	 *   4WAY_HANDSHAKE → GROUP_HANDSHAKE → COMPLETED
	 * We treat COMPLETED (+ optional ip_address) as success; any
	 * other terminal state on timeout is "still trying". Polling is
	 * simpler and more portable than subscribing to the wpa_cli
	 * event socket, and 500 ms keeps the worst-case UI latency
	 * under a second once the radio has associated. */
	deadline := time.Now().Add(20 * time.Second)
	var (
		state, ssid, ip string
		connected       bool
	)
	for time.Now().Before(deadline) {
		out, err := exec.Command("wpa_cli", "-i", "wlan0", "status").Output()
		if err == nil {
			state, ssid, ip = parseWpaStatus(out)
			if state == "COMPLETED" {
				connected = true
				/* Give DHCP a short grace period to populate
				 * ip_address so the UI can show it in the
				 * success message. A successful association
				 * without an IP yet is still reported as
				 * connected — the user can refresh later. */
				if ip != "" {
					break
				}
			}
		}
		time.Sleep(500 * time.Millisecond)
	}

	log.Printf("wifi: connect result state=%s ssid=%q ip=%s connected=%v",
		state, ssid, ip, connected)
	jsonResponse(w, http.StatusOK, map[string]interface{}{
		"connected": connected,
		"state":     state,
		"ssid":      ssid,
		"ip":        ip,
	})
}

// PostChangePassword changes the pi-clock user's password.
// Verifies the current password first, then uses passwd(1).
// Destroys the session on success so the user must re-authenticate.
func PostChangePassword(sessions *auth.SessionStore) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		var req struct {
			Current string `json:"current"`
			New     string `json:"new"`
		}
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
			jsonResponse(w, http.StatusBadRequest, map[string]string{"error": "invalid JSON"})
			return
		}

		if req.Current == "" || req.New == "" {
			jsonResponse(w, http.StatusBadRequest, map[string]string{"error": "both fields required"})
			return
		}

		/* Reject newlines, colons, and null bytes — these could inject
		 * extra records into chpasswd's "user:pass" stdin format. */
		if strings.ContainsAny(req.New, "\n\r\x00:") {
			jsonResponse(w, http.StatusBadRequest, map[string]string{
				"error": "invalid characters in new password",
			})
			return
		}
		for _, v := range []string{req.Current, req.New} {
			if strings.ContainsAny(v, "\n\r\x00") {
				jsonResponse(w, http.StatusBadRequest, map[string]string{
					"error": "invalid characters in password",
				})
				return
			}
		}

		if len(req.New) < 8 {
			jsonResponse(w, http.StatusBadRequest, map[string]string{
				"error": "password too short (min 8 characters)",
			})
			return
		}

		/* Verify the current password first */
		if !auth.VerifyPassword("pi-clock", req.Current) {
			log.Printf("password change: wrong current password from %s", r.RemoteAddr)
			jsonResponse(w, http.StatusForbidden, map[string]string{
				"error": "current password is incorrect",
			})
			return
		}

		/* Change via chpasswd (reads "user:pass" from stdin) */
		cmd := exec.Command("chpasswd")
		cmd.Stdin = strings.NewReader("pi-clock:" + req.New + "\n")
		if err := cmd.Run(); err != nil {
			log.Printf("password change: chpasswd failed: %v", err)
			jsonResponse(w, http.StatusInternalServerError, map[string]string{
				"error": "password change failed",
			})
			return
		}

		log.Printf("password change: pi-clock password changed from %s", r.RemoteAddr)

		/* Persist the new hash to pi-clock-user.conf so it survives A/B
		 * OS upgrades. pi-clock-config reads this at boot and writes
		 * the hash into /etc/shadow on the fresh rootfs. */
		hash, err := auth.GetShadowHash("pi-clock")
		if err != nil {
			log.Printf("password change: failed to read new hash: %v", err)
		} else if err := updateUserConf("user_password_hash", hash); err != nil {
			log.Printf("password change: failed to persist hash: %v", err)
		} else {
			log.Printf("password change: hash persisted to %s", userConfPath)
		}

		/* Destroy ALL sessions so any hijacked tokens are invalidated */
		sessions.DestroyAll()
		sessions.Destroy(w, r)

		jsonResponse(w, http.StatusOK, map[string]string{"status": "password changed"})
	}
}

// Applet definitions — name, display label, defaults
var appletDefs = []struct {
	Name    string
	Label   string
	Default bool
	Side    string
}{
	{"dxfeed", "DX Cluster Feed", false, "right"},
	{"muf", "MUF Estimate", false, "left"},
	{"voacap", "Propagation Prediction", false, "right"},
	{"solar", "Solar Weather", false, "left"},
	{"sysinfo", "System Info", true, "left"},
	{"features", "Active Features", true, "right"},
}

const appletsConfPath = "/data/etc/pi-clock-applets.conf"

// GetApplets returns the current applet settings merged with compiled-in
// defaults, in the order the user has arranged them.
//
// Ordering contract: the line order in pi-clock-applets.conf is the
// visual stacking order (first-in-file = top-of-panel within its side,
// see pic_applet_load_config() in the renderer). The dashboard's
// drag-and-drop UI sends applets in the intended visual order, and
// this handler returns them in that same order so a page refresh
// always shows the last-saved arrangement.
//
// Any applet defined in appletDefs but not present in the saved file
// (a fresh install, or a newly-added applet the user hasn't dragged
// yet) is appended to the tail with its compiled-in defaults. This
// keeps new applets discoverable rather than quietly missing.
func GetApplets() http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		savedOrder, savedByName := readAppletsConf()

		/* Label/default lookup keyed by applet name. A name we
		 * don't recognise (e.g. a config line left over from a
		 * removed applet) is skipped entirely — better to drop it
		 * than to round-trip unknown entries the UI can't render. */
		defByName := make(map[string]int, len(appletDefs))
		for i := range appletDefs {
			defByName[appletDefs[i].Name] = i
		}

		applets := make([]map[string]interface{}, 0, len(appletDefs))
		emitted := make(map[string]bool, len(appletDefs))

		/* First: walk the saved file in its own order. */
		for _, name := range savedOrder {
			di, ok := defByName[name]
			if !ok {
				continue
			}
			s := savedByName[name]
			applets = append(applets, map[string]interface{}{
				"name":    appletDefs[di].Name,
				"label":   appletDefs[di].Label,
				"enabled": s.enabled,
				"side":    s.side,
			})
			emitted[name] = true
		}

		/* Then: append any applet we know about that the saved
		 * file didn't mention, in the compiled-in declaration
		 * order. New applets land at the end of their side's
		 * visual stack — least surprising. */
		for _, def := range appletDefs {
			if emitted[def.Name] {
				continue
			}
			applets = append(applets, map[string]interface{}{
				"name":    def.Name,
				"label":   def.Label,
				"enabled": def.Default,
				"side":    def.Side,
			})
		}

		jsonResponse(w, http.StatusOK, applets)
	}
}

// PutApplets writes the applet configuration in the exact order the
// request body specifies. That line order is the render stacking order
// — see GetApplets / pic_applet_load_config for the contract.
//
// Unknown applet names are silently dropped (validName guard) so a
// malicious or malformed request can't inject arbitrary keys into the
// config file.
func PutApplets() http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		var applets []struct {
			Name    string `json:"name"`
			Enabled bool   `json:"enabled"`
			Side    string `json:"side"`
		}

		if err := json.NewDecoder(r.Body).Decode(&applets); err != nil {
			jsonResponse(w, http.StatusBadRequest, map[string]string{
				"error": "invalid JSON",
			})
			return
		}

		/* Guard against two request bugs by dropping anything
		 * we don't recognise from appletDefs — a client that
		 * sends a renamed or fabricated name shouldn't be able
		 * to create config entries the renderer can't consume. */
		known := make(map[string]bool, len(appletDefs))
		for _, d := range appletDefs {
			known[d.Name] = true
		}

		lines := []string{
			"# Pi-Clock applet configuration",
			"# Managed by the dashboard.",
			"# Line order = visual stacking order within each side.",
		}
		for _, a := range applets {
			if !validName.MatchString(a.Name) || !known[a.Name] {
				continue
			}
			e := "0"
			if a.Enabled {
				e = "1"
			}
			side := a.Side
			if side != "left" && side != "right" {
				/* Disabled applets still carry a side in the
				 * file so the renderer has a stable answer for
				 * "which column would this land in if re-enabled".
				 * Default to right for historical compatibility. */
				side = "right"
			}
			lines = append(lines, fmt.Sprintf("%s=%s,%s", a.Name, e, side))
		}

		content := strings.Join(lines, "\n") + "\n"
		if err := atomicWriteFile(appletsConfPath, []byte(content), 0644); err != nil {
			internalError(w, "save applets", err)
			return
		}

		signalRenderer()

		jsonResponse(w, http.StatusOK, map[string]string{
			"status": "saved",
		})
	}
}

type appletSetting struct {
	enabled bool
	side    string
}

// readAppletsConf parses the saved applet config and returns the
// names in file-order plus a lookup map of settings. Callers that
// care about order walk the slice; callers that only need values
// use the map.
func readAppletsConf() (order []string, byName map[string]appletSetting) {
	byName = make(map[string]appletSetting)

	data, err := os.ReadFile(appletsConfPath)
	if err != nil {
		return nil, byName
	}

	for _, line := range strings.Split(string(data), "\n") {
		line = strings.TrimSpace(line)
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}

		parts := strings.SplitN(line, "=", 2)
		if len(parts) != 2 {
			continue
		}

		name := parts[0]
		if !validName.MatchString(name) {
			continue
		}
		vals := strings.SplitN(parts[1], ",", 2)

		enabled := vals[0] == "1"
		side := "right"
		if len(vals) == 2 {
			side = strings.TrimSpace(vals[1])
		}

		/* Skip duplicates of the same applet (malformed file)
		 * so the first occurrence wins — keeps the order slice
		 * in sync with the map and avoids double-rendering. */
		if _, dup := byName[name]; dup {
			continue
		}
		order = append(order, name)
		byName[name] = appletSetting{enabled: enabled, side: side}
	}

	return order, byName
}

// GetDxSettings returns current DX cluster filter settings from the config file.
func GetDxSettings() http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		confPath := "/data/etc/pi-clock-renderer.conf"
		data, _ := os.ReadFile(confPath)

		result := map[string]string{
			"dx_distance": "0",
			"dx_bands":    "0FFF",
			"dx_spot_age": "900",
		}

		for _, line := range strings.Split(string(data), "\n") {
			if strings.HasPrefix(line, "DX_DISTANCE=") {
				result["dx_distance"] = strings.TrimSpace(line[12:])
			}
			if strings.HasPrefix(line, "DX_BANDS=") {
				result["dx_bands"] = strings.TrimSpace(line[9:])
			}
			if strings.HasPrefix(line, "DX_SPOT_AGE=") {
				result["dx_spot_age"] = strings.TrimSpace(line[12:])
			}
		}

		jsonResponse(w, http.StatusOK, result)
	}
}

// Satellite definitions — must match the renderer's satellite list in display.c
var satelliteDefs = []struct {
	Name    string
	Label   string
	NoradID int
	Default bool
}{
	{"ao7", "AO-7 — Linear, sun-only", 7530, false},
	{"ao73", "AO-73 (FUNcube-1) — Linear", 39444, false},
	{"ao91", "AO-91 (Fox-1B) — FM, sun-only", 43017, false},
	{"ao123", "AO-123 (ASRTU-1) — FM", 61781, false},
	{"iss", "ISS (ZARYA)", 25544, false},
	{"jo97", "JO-97 (FUNcube-6) — Linear", 43803, false},
	{"meteor_m2", "Meteor-M 2 — LRPT", 40069, false},
	{"meteor_m22", "Meteor-M2-2 — LRPT", 44387, false},
	{"meteor_m23", "Meteor-M2-3 — LRPT", 57166, false},
	{"meteor_m24", "Meteor-M2-4 — LRPT", 59051, false},
	{"po101", "PO-101 (Diwata-2) — FM", 43678, false},
	{"qo100", "QO-100 (Es'hail-2) — GEO", 43700, false},
	{"rs44", "RS-44 (DOSAAF-85) — Linear", 44909, false},
	{"so50", "SO-50 (SaudiSat) — FM", 27607, false},
	{"so125", "SO-125 (HADES-ICM) — FM/Digi", 63492, false},
	{"sonate2", "SONATE-2 — APRS Digi", 59112, false},
}

const satConfPath = "/data/etc/pi-clock-satellites.conf"

func GetSatellites() http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		saved := readSatConf()
		var sats []map[string]interface{}
		for _, def := range satelliteDefs {
			enabled := def.Default
			if s, ok := saved[def.Name]; ok {
				enabled = s
			}
			sats = append(sats, map[string]interface{}{
				"name":    def.Name,
				"label":   def.Label,
				"enabled": enabled,
			})
		}
		jsonResponse(w, http.StatusOK, sats)
	}
}

func PutSatellites() http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		var sats []struct {
			Name    string `json:"name"`
			Enabled bool   `json:"enabled"`
		}
		if err := json.NewDecoder(r.Body).Decode(&sats); err != nil {
			jsonResponse(w, http.StatusBadRequest, map[string]string{"error": "invalid JSON"})
			return
		}

		var lines []string
		lines = append(lines, "# Pi-Clock satellite configuration")
		for _, s := range sats {
			if !validName.MatchString(s.Name) {
				continue
			}
			e := "0"
			if s.Enabled { e = "1" }
			lines = append(lines, fmt.Sprintf("%s=%s", s.Name, e))
		}

		content := strings.Join(lines, "\n") + "\n"
		if err := atomicWriteFile(satConfPath, []byte(content), 0644); err != nil {
			internalError(w, "save satellites", err)
			return
		}
		signalRenderer()
		jsonResponse(w, http.StatusOK, map[string]string{"status": "saved"})
	}
}

func readSatConf() map[string]bool {
	result := make(map[string]bool)
	data, err := os.ReadFile(satConfPath)
	if err != nil { return result }
	for _, line := range strings.Split(string(data), "\n") {
		line = strings.TrimSpace(line)
		if line == "" || strings.HasPrefix(line, "#") { continue }
		parts := strings.SplitN(line, "=", 2)
		if len(parts) == 2 && validName.MatchString(parts[0]) {
			result[parts[0]] = parts[1] == "1"
		}
	}
	return result
}

// Ticker source definitions — must match the renderer's sources in display.c
var tickerSourceDefs = []struct {
	Name    string
	Label   string
	Default bool
}{
	{"noaa_alerts", "NOAA Space Weather Alerts", false},
	{"dxworld", "DX World News", false},
	{"arrl", "ARRL News", false},
	{"rsgb", "RSGB GB2RS News", false},
	{"southgate", "Southgate ARC News", false},
}

const tickerConfPath = "/data/etc/pi-clock-ticker.conf"

// GetTickerSources returns current ticker source settings and mode.
func GetTickerSources() http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		saved, mode := readTickerConf()

		var sources []map[string]interface{}
		for _, def := range tickerSourceDefs {
			enabled := def.Default
			if s, ok := saved[def.Name]; ok {
				enabled = s
			}
			sources = append(sources, map[string]interface{}{
				"name":    def.Name,
				"label":   def.Label,
				"enabled": enabled,
			})
		}
		jsonResponse(w, http.StatusOK, map[string]interface{}{
			"sources": sources,
			"mode":    mode,
		})
	}
}

// PutTickerSources saves ticker source settings and mode.
func PutTickerSources() http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		var req struct {
			Sources []struct {
				Name    string `json:"name"`
				Enabled bool   `json:"enabled"`
			} `json:"sources"`
			Mode int `json:"mode"`
		}
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
			jsonResponse(w, http.StatusBadRequest, map[string]string{
				"error": "invalid JSON",
			})
			return
		}

		var lines []string
		lines = append(lines, "# Pi-Clock ticker source configuration")
		lines = append(lines, fmt.Sprintf("mode=%d", req.Mode))
		for _, s := range req.Sources {
			if !validName.MatchString(s.Name) {
				continue
			}
			e := "0"
			if s.Enabled {
				e = "1"
			}
			lines = append(lines, fmt.Sprintf("%s=%s", s.Name, e))
		}

		content := strings.Join(lines, "\n") + "\n"
		if err := atomicWriteFile(tickerConfPath, []byte(content), 0644); err != nil {
			internalError(w, "save ticker", err)
			return
		}

		signalRenderer()
		jsonResponse(w, http.StatusOK, map[string]string{"status": "saved"})
	}
}

func readTickerConf() (map[string]bool, int) {
	result := make(map[string]bool)
	mode := 1 /* default: headlines + full news */
	data, err := os.ReadFile(tickerConfPath)
	if err != nil {
		return result, mode
	}
	for _, line := range strings.Split(string(data), "\n") {
		line = strings.TrimSpace(line)
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		parts := strings.SplitN(line, "=", 2)
		if len(parts) == 2 {
			if parts[0] == "mode" {
				fmt.Sscanf(parts[1], "%d", &mode)
			} else {
				result[parts[0]] = parts[1] == "1"
			}
		}
	}
	return result, mode
}

// PutDxSettings saves DX cluster filter settings to the renderer config.
// Takes the RendererConfig to acquire the write lock — prevents data
// races with PutConfig which writes to the same file.
func PutDxSettings(cfg *config.RendererConfig) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		var settings struct {
			DxDistance string `json:"dx_distance"`
			DxBands   string `json:"dx_bands"`
			DxSpotAge string `json:"dx_spot_age"`
		}

		if err := json.NewDecoder(r.Body).Decode(&settings); err != nil {
			jsonResponse(w, http.StatusBadRequest, map[string]string{
				"error": "invalid JSON",
			})
			return
		}

		// Validate DX values: distance and age must be digits only,
		// bands must be hex. Prevents config file injection.
		for _, v := range []string{settings.DxDistance, settings.DxBands, settings.DxSpotAge} {
			if !validDxValue.MatchString(v) {
				jsonResponse(w, http.StatusBadRequest, map[string]string{
					"error": "invalid characters in DX settings",
				})
				return
			}
		}

		// Hold the renderer config lock to prevent races with PutConfig
		cfg.Lock()
		defer cfg.Unlock()

		// Read existing config, update DX fields, write back
		confPath := "/data/etc/pi-clock-renderer.conf"
		data, _ := os.ReadFile(confPath)

		// Remove existing DX_ lines
		var lines []string
		for _, line := range strings.Split(string(data), "\n") {
			if !strings.HasPrefix(line, "DX_DISTANCE=") &&
				!strings.HasPrefix(line, "DX_BANDS=") &&
				!strings.HasPrefix(line, "DX_SPOT_AGE=") {
				lines = append(lines, line)
			}
		}

		// Add new values
		lines = append(lines, "DX_DISTANCE="+settings.DxDistance)
		lines = append(lines, "DX_BANDS="+settings.DxBands)
		lines = append(lines, "DX_SPOT_AGE="+settings.DxSpotAge)

		if err := atomicWriteFile(confPath, []byte(strings.Join(lines, "\n")), 0644); err != nil {
			internalError(w, "save dx settings", err)
			return
		}

		// Signal renderer
		signalRenderer()

		jsonResponse(w, http.StatusOK, map[string]string{
			"status": "saved",
		})
	}
}

// GetPropSettings returns the propagation band mask from the renderer config.
func GetPropSettings() http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		result := map[string]string{
			"prop_bands":   "FF",
			"antenna_gain": "0",
		}
		data, err := os.ReadFile("/data/etc/pi-clock-renderer.conf")
		if err == nil {
			for _, line := range strings.Split(string(data), "\n") {
				parts := strings.SplitN(strings.TrimSpace(line), "=", 2)
				if len(parts) != 2 {
					continue
				}
				switch parts[0] {
				case "PROP_BANDS":
					if validDxValue.MatchString(parts[1]) {
						result["prop_bands"] = parts[1]
					}
				case "ANTENNA_GAIN":
					if g, err := strconv.Atoi(parts[1]); err == nil && g >= -12 && g <= 12 {
						result["antenna_gain"] = parts[1]
					}
				}
			}
		}
		jsonResponse(w, http.StatusOK, result)
	}
}

// PutPropSettings saves the propagation band mask to the renderer config.
func PutPropSettings(cfg *config.RendererConfig) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		var req struct {
			PropBands   string `json:"prop_bands"`
			AntennaGain string `json:"antenna_gain"`
		}
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
			jsonResponse(w, http.StatusBadRequest, map[string]string{"error": "invalid JSON"})
			return
		}
		if !validDxValue.MatchString(req.PropBands) || req.PropBands == "" {
			jsonResponse(w, http.StatusBadRequest, map[string]string{"error": "invalid band mask"})
			return
		}

		/* Validate antenna gain as integer -12 to +12 */
		gain, err := strconv.Atoi(req.AntennaGain)
		if err != nil || gain < -12 || gain > 12 {
			jsonResponse(w, http.StatusBadRequest, map[string]string{"error": "antenna gain must be -12 to +12"})
			return
		}
		gainStr := strconv.Itoa(gain) /* sanitised integer string */

		cfg.Lock()
		defer cfg.Unlock()

		confPath := "/data/etc/pi-clock-renderer.conf"
		data, _ := os.ReadFile(confPath)
		lines := strings.Split(string(data), "\n")

		/* Update or add PROP_BANDS and ANTENNA_GAIN */
		foundBands := false
		foundGain := false
		for i, line := range lines {
			trimmed := strings.TrimSpace(line)
			if strings.HasPrefix(trimmed, "PROP_BANDS=") {
				lines[i] = "PROP_BANDS=" + req.PropBands
				foundBands = true
			}
			if strings.HasPrefix(trimmed, "ANTENNA_GAIN=") {
				lines[i] = "ANTENNA_GAIN=" + gainStr
				foundGain = true
			}
		}
		if !foundBands {
			lines = append(lines, "PROP_BANDS="+req.PropBands)
		}
		if !foundGain {
			lines = append(lines, "ANTENNA_GAIN="+gainStr)
		}
		if err := atomicWriteFile(confPath, []byte(strings.Join(lines, "\n")), 0644); err != nil {
			internalError(w, "save prop settings", err)
			return
		}
		signalRenderer()
		jsonResponse(w, http.StatusOK, map[string]string{"status": "saved"})
	}
}

// ── Lightning Settings ─────────────────────────────────────────────

// GetLightningSettings returns the lightning fade time from the renderer config.
func GetLightningSettings() http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		confPath := "/data/etc/pi-clock-renderer.conf"
		data, _ := os.ReadFile(confPath)

		result := map[string]string{
			"lightning_fade_ms": "15000",
		}

		for _, line := range strings.Split(string(data), "\n") {
			if strings.HasPrefix(line, "LIGHTNING_FADE_MS=") {
				result["lightning_fade_ms"] = strings.TrimSpace(line[18:])
			}
		}

		jsonResponse(w, http.StatusOK, result)
	}
}

// PutLightningSettings saves the lightning fade time to the renderer config.
func PutLightningSettings(cfg *config.RendererConfig) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		var req map[string]string
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
			jsonResponse(w, http.StatusBadRequest, map[string]string{"error": "invalid JSON"})
			return
		}

		fadeVal, ok := req["lightning_fade_ms"]
		if !ok || fadeVal == "" {
			jsonResponse(w, http.StatusBadRequest, map[string]string{"error": "missing fade value"})
			return
		}
		fadeMs, err := strconv.ParseInt(fadeVal, 10, 64)
		if err != nil || fadeMs < 5000 || fadeMs > 120000 {
			jsonResponse(w, http.StatusBadRequest, map[string]string{"error": "fade must be 5000-120000 ms"})
			return
		}

		cfg.Lock()
		defer cfg.Unlock()

		confPath := "/data/etc/pi-clock-renderer.conf"
		data, _ := os.ReadFile(confPath)
		lines := strings.Split(string(data), "\n")

		found := false
		for i, line := range lines {
			if strings.HasPrefix(strings.TrimSpace(line), "LIGHTNING_FADE_MS=") {
				lines[i] = "LIGHTNING_FADE_MS=" + fadeVal
				found = true
			}
		}
		if !found {
			lines = append(lines, "LIGHTNING_FADE_MS="+fadeVal)
		}
		if err := atomicWriteFile(confPath, []byte(strings.Join(lines, "\n")), 0644); err != nil {
			internalError(w, "save lightning settings", err)
			return
		}
		signalRenderer()
		jsonResponse(w, http.StatusOK, map[string]string{"status": "saved"})
	}
}

// ── Earthquake Settings ────────────────────────────────────────────

// GetEarthquakeSettings returns the earthquake display time from the renderer config.
func GetEarthquakeSettings() http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		confPath := "/data/etc/pi-clock-renderer.conf"
		data, _ := os.ReadFile(confPath)

		result := map[string]string{
			"earthquake_display_s": "86400",
		}

		for _, line := range strings.Split(string(data), "\n") {
			if strings.HasPrefix(line, "EARTHQUAKE_DISPLAY_S=") {
				result["earthquake_display_s"] = strings.TrimSpace(line[21:])
			}
		}

		jsonResponse(w, http.StatusOK, result)
	}
}

// PutEarthquakeSettings saves the earthquake display time to the renderer config.
func PutEarthquakeSettings(cfg *config.RendererConfig) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		var req map[string]string
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
			jsonResponse(w, http.StatusBadRequest, map[string]string{"error": "invalid JSON"})
			return
		}

		displayVal, ok := req["earthquake_display_s"]
		if !ok || displayVal == "" {
			jsonResponse(w, http.StatusBadRequest, map[string]string{"error": "missing display time"})
			return
		}
		displaySec, err := strconv.ParseInt(displayVal, 10, 64)
		if err != nil || displaySec < 60 || displaySec > 86400 {
			jsonResponse(w, http.StatusBadRequest, map[string]string{"error": "display time must be 60-86400 s"})
			return
		}

		cfg.Lock()
		defer cfg.Unlock()

		confPath := "/data/etc/pi-clock-renderer.conf"
		data, _ := os.ReadFile(confPath)
		lines := strings.Split(string(data), "\n")

		found := false
		for i, line := range lines {
			if strings.HasPrefix(strings.TrimSpace(line), "EARTHQUAKE_DISPLAY_S=") {
				lines[i] = "EARTHQUAKE_DISPLAY_S=" + displayVal
				found = true
			}
		}
		if !found {
			lines = append(lines, "EARTHQUAKE_DISPLAY_S="+displayVal)
		}
		if err := atomicWriteFile(confPath, []byte(strings.Join(lines, "\n")), 0644); err != nil {
			internalError(w, "save earthquake settings", err)
			return
		}
		signalRenderer()
		jsonResponse(w, http.StatusOK, map[string]string{"status": "saved"})
	}
}

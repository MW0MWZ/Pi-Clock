# Pi-Clock TODO

## Build Pipeline
- [x] Tag-driven APK builds — trigger on `v*` tag push, version from tag
- [x] APK build completion auto-triggers OS image build via `workflow_call`
- [x] Package the Go dashboard as its own APK
- [x] Add `pi-clock-dashboard` init script to OS overlay service list
- [x] Boot splash binary + init script packaged in renderer APK

## Dashboard
- [x] Layer control panel — toggle switches and opacity sliders
- [x] Shutdown / reboot / rollback buttons
- [x] APK upgrade and OS upgrade with progress feedback
- [x] Show display resolution, refresh rate, CPU cores, Pi model
- [x] Resolution override — hardware-aware limits (1080p cap for Pi 0/1/2)
- [x] Resolution changes write config.txt directly (single reboot)
- [x] Browser geolocation for QTH
- [x] Auto-calculate grid square from lat/lon on save
- [x] DX cluster settings (distance, bands, spot age)
- [x] Band toggle buttons with contrast-aware text
- [x] Applet controls — toggle on/off, pick side (left/right)
- [x] Satellite selection panel
- [x] News ticker source toggles and mode selector
- [x] WiFi configuration (SSID, password, country)
- [x] Password change
- [x] Hostname change from dashboard
- [x] Logo branding — navbar, login, reboot/shutdown overlay
- [x] Light/dark themes with logo-derived colour scheme (charcoal + gold)
- [x] Mobile responsive improvements

## Renderer — Completed Layers
- [x] Base map (Blue Marble / Black Marble day/night blend)
- [x] Day/night terminator with latitude/season-dependent width
- [x] Country borders (Natural Earth 110m)
- [x] Lat/lon grid (10-degree intervals)
- [x] Time zones (real Natural Earth boundaries, UTC offset labels)
- [x] CQ Zones (real HB9HIL boundaries, zone labels with nudges)
- [x] Sun position marker
- [x] Moon position + phase display
- [x] Maidenhead grid square overlay (2-char fields)
- [x] HUD — UTC clock (left) + local time with TZ (right)
- [x] Map centering on any longitude with wrapping
- [x] DX Cluster spots — great circle arcs, band colours, label pills
- [x] DX distance filter, band filter, spot aging (all configurable)
- [x] QTH marker (home location pin with callsign label)
- [x] Satellite tracking — SGP4 propagation, ground tracks, footprints
- [x] News ticker — headlines + full news (chunked) and smooth scroll modes

## Renderer — Completed Applets
- [x] Applet framework — stackable overlay panels with dark backgrounds
- [x] Applet min-width system with resolution scaling
- [x] DX Cluster Feed applet (scrolling live feed)
- [x] MUF Estimate applet (real solar position, live SFI)
- [x] Solar Weather applet (SFI, Kp, wind, Bz, X-ray, SDO images, band predictions)
- [x] System Info applet (hostname, IP, OS, arch, cores, display, uptime, version)
- [x] Active Features applet (lists enabled layers and panels)

## Renderer — Performance
- [x] Two-tier compositing cache (28% → 5% CPU at 4K)
- [x] Ticker skipped in main composite (own thread)
- [x] Resolution-adaptive daylight step (4/6/8 by resolution)
- [x] Dirty flag tracking — skip composite when nothing changed
- [x] Per-source ticker item cap (prevents feed starvation)
- [x] Precise clock timing — sleep to next second boundary (no drift)
- [x] Tiered NOAA fetch intervals (30min/2hr/6hr instead of all-at-4hr)
- [x] All network threads at nice(15) to prevent renderer starvation
- [x] Bulk TLE from AMSAT (one request instead of 20+)
- [x] Tmpfs caching for solar data, SDO images, TLEs

## Renderer — Next
- [x] Propagation prediction overlay (heat map per band, inspired by MUF contour/isobar concept)
- [ ] GPU compositing via DRM/KMS (further CPU reduction)
- [ ] ITU zone overlay
- [ ] DXCC entity boundaries
- [ ] POTA/SOTA active spots
- [ ] Aurora oval (NOAA SWPC API)
- [ ] WSJT-X integration (local UDP)
- [ ] RBN integration
- [ ] PSKReporter / FT8
- [x] Better callsign geolocation for DX spots — callsign district lookup table for US, Canada, Australia, Russia, Japan, Brazil, Italy, Spain

## OS / Boot
- [x] Framebuffer auto-detection from EDID
- [x] A/B slot upgrades working
- [x] Dashboard starts on boot
- [x] Boot splash with Pi-Clock logo
- [x] Password persistence across OS upgrades
- [x] Tested on Pi Zero W, Zero 2 W, Pi 2, Pi 3A+, Pi 4, Pi 5
- [x] mDNS responder for pi-clock.local discovery

## Website (gh-pages)
- [x] Project landing page with logo-derived theme
- [x] Config generator tool (WiFi, callsign, location, SSH)
- [x] Auto-detect location/timezone/country from browser
- [x] Getting started guide (image → SD card → boot → dashboard)
- [x] APK repository with signed packages

## Polish
- [x] Pi-Clock logo branding throughout (boot, dashboard, website)
- [x] Sensible fresh-install defaults (minimal layers on, user enables via dashboard)
- [ ] Improve greyline visibility (optional terminator line)

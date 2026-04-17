/*
 * dashboard.js - Dashboard interaction logic
 *
 * Handles form submission, API calls, and live updates
 * for the Pi-Clock dashboard. Vanilla JS, no framework.
 *
 * All dynamic HTML is built with DOM API (createElement/textContent)
 * rather than innerHTML to prevent XSS from config-file injection.
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

// ── RAM budget ─────────────────────────────────────────────
// These are populated from /api/system/info when the page loads.
// ramBudgetMb is the MB available for layer surfaces (total - overhead).
// surfaceMb is calculated from display resolution (w * h * 4 / 1M).
var ramBudgetMb = 0;
var surfaceMb = 0;

/* Per-layer extra memory cost in MB beyond the surface.
 * Must match the layer_budget[] table in display.c. */
var layerExtraMb = {
    'wind':           4,
    'aurora':         4,
    'satellites':     2,
    'lightning':      2,
    'dxspots':        1,
    'bandconditions': 1,
    'ticker':         1,
    'earthquakes':    1
};

/* Calculate total memory cost of all enabled layers */
function calcLayerCostMb() {
    var total = 0;
    document.querySelectorAll('[data-layer]').forEach(function(toggle) {
        if (toggle.checked) {
            var name = toggle.getAttribute('data-layer');
            total += surfaceMb + (layerExtraMb[name] || 0);
        }
    });
    /* Also count always-on layers (basemap, daylight) that have
     * no toggle checkbox — they have opacity sliders only */
    total += surfaceMb * 2;
    return total;
}

/* Update the RAM budget warning banner visibility */
function updateRamWarning() {
    var banner = document.getElementById('ram-warning');
    if (!banner || ramBudgetMb <= 0) return;

    var used = calcLayerCostMb();
    if (used > ramBudgetMb) {
        banner.textContent = 'RAM warning: enabled layers need ~' +
            used + 'MB but only ' + ramBudgetMb +
            'MB is available. The renderer will force-disable ' +
            'low-priority layers to prevent a crash.';
        banner.style.display = '';
    } else {
        banner.style.display = 'none';
    }
}

// ── API helper ──────────────────────────────────────────────
function apiCall(method, url, data) {
    var opts = {
        method: method,
        headers: {
            'Content-Type': 'application/json',
            /* X-Requested-With marks this as an AJAX request. Provides
             * minor CSRF mitigation via same-origin policy for custom
             * headers, though SameSite=Strict on the session cookie is
             * our primary CSRF defence. */
            'X-Requested-With': 'XMLHttpRequest'
        },
        credentials: 'same-origin'
    };
    if (data) opts.body = JSON.stringify(data);
    return fetch(url, opts).then(function(r) {
        if (r.status === 401) {
            window.location.href = '/login';
            throw new Error('unauthorized');
        }
        if (!r.ok) {
            return r.json().catch(function() { return {}; }).then(function(body) {
                throw new Error(body.error || 'Request failed');
            });
        }
        return r.json();
    });
}

// ── DOM builder helpers ─────────────────────────────────────
// Safe element builders — never use innerHTML with external data.

/* Build a toggle switch (checkbox + slider) */
function buildToggle(dataAttr, dataVal, checked) {
    var label = document.createElement('label');
    label.className = 'layer-toggle';

    var input = document.createElement('input');
    input.type = 'checkbox';
    input.setAttribute(dataAttr, dataVal);
    if (checked) input.checked = true;

    var slider = document.createElement('span');
    slider.className = 'slider';

    label.appendChild(input);
    label.appendChild(slider);
    return label;
}

/* Build a text label span */
function buildLabel(text) {
    var span = document.createElement('span');
    span.className = 'layer-name';
    span.textContent = text;
    return span;
}

/* Build a toggle row: switch + label */
function buildToggleRow(dataAttr, name, label, enabled) {
    var row = document.createElement('div');
    row.className = 'layer-row';
    row.appendChild(buildToggle(dataAttr, name, enabled));
    row.appendChild(buildLabel(label));
    return row;
}

// ── Browser geolocation ─────────────────────────────────────
function useMyLocation() {
    if (!navigator.geolocation) {
        toast('Geolocation not supported by your browser', 'error');
        return;
    }

    navigator.geolocation.getCurrentPosition(function(pos) {
        var lat = pos.coords.latitude.toFixed(4);
        var lon = pos.coords.longitude.toFixed(4);
        var grid = latLonToGrid(pos.coords.latitude, pos.coords.longitude);

        document.getElementById('qth_lat').value = lat;
        document.getElementById('qth_lon').value = lon;
        document.getElementById('center_lon').value = lon;
        document.getElementById('grid_square').value = grid;

        toast('Location set: ' + lat + ', ' + lon + ' (' + grid + ')');
    }, function(err) {
        toast('Location access denied: ' + err.message, 'error');
    });
}

// ── Maidenhead grid square calculation ──────────────────────
// Converts WGS-84 lat/lon to a 6-character Maidenhead grid locator
// (e.g. "IO83ro"). Used universally in amateur radio for station
// identification in contest exchanges and propagation reports.
//
// The system divides Earth into three nested levels:
//   Field    — 20° lon × 10° lat  (letters A-R, e.g. "IO")
//   Square   — 2° lon × 1° lat    (digits 0-9, e.g. "83")
//   Subsquare — 1/12° × 1/24°     (letters a-x, e.g. "ro")
//
// Origin is at 180°W / 90°S (south-west corner of field AA).
// charCode(65)='A' for fields, charCode(97)='a' for subsquares.
function latLonToGrid(lat, lon) {
    lon = lon + 180;
    lat = lat + 90;

    var field_lon = Math.floor(lon / 20);
    var field_lat = Math.floor(lat / 10);
    var square_lon = Math.floor((lon % 20) / 2);
    var square_lat = Math.floor(lat % 10);
    var sub_lon = Math.floor((lon % 2) * 12);
    var sub_lat = Math.floor((lat % 1) * 24);

    return String.fromCharCode(65 + field_lon) +
           String.fromCharCode(65 + field_lat) +
           square_lon.toString() +
           square_lat.toString() +
           String.fromCharCode(97 + sub_lon) +
           String.fromCharCode(97 + sub_lat);
}

// ── Toast notifications ─────────────────────────────────────
/* Get or create the toast stack container. All toasts and the
 * persistent reboot card live here, stacking bottom-up. */
function getToastStack() {
    var stack = document.getElementById('toast-stack');
    if (!stack) {
        stack = document.createElement('div');
        stack.id = 'toast-stack';
        stack.className = 'toast-stack';
        document.body.appendChild(stack);
    }
    return stack;
}

function toast(message, type) {
    var el = document.createElement('div');
    el.className = 'toast toast-' + (type || 'success');
    el.textContent = message;
    getToastStack().appendChild(el);
    setTimeout(function() { el.remove(); }, 3000);
}

// ── Helpers ─────────────────────────────────────────────────
function val(id) {
    var el = document.getElementById(id);
    return el ? el.value : '';
}

function setText(id, text) {
    var el = document.getElementById(id);
    if (el) el.textContent = text;
}

function formatUptime(seconds) {
    var s = parseFloat(seconds);
    if (isNaN(s)) return '-';
    var d = Math.floor(s / 86400);
    var h = Math.floor((s % 86400) / 3600);
    var m = Math.floor((s % 3600) / 60);
    if (d > 0) return d + 'd ' + h + 'h ' + m + 'm';
    if (h > 0) return h + 'h ' + m + 'm';
    return m + 'm';
}

function scrollToLayers() {
    var el = document.getElementById('layer-controls');
    if (el) el.scrollIntoView({behavior: 'smooth', block: 'center'});
}

function setCardVisibility(id, visible) {
    var card = document.getElementById(id);
    if (card) card.style.display = visible ? '' : 'none';
}

// ── Load system info ────────────────────────────────────────
function loadSystemInfo() {
    apiCall('GET', '/api/system/info').then(function(info) {
        setText('sys-hostname', info.hostname || '-');
        setText('sys-uptime', formatUptime(info.uptime));
        setText('sys-arch', info.arch || '-');
        setText('sys-display', info.display_size ?
            info.display_size.replace(',', 'x') : '-');
        setText('sys-refresh', info.refresh_rate || '-');
        setText('sys-cores', info.cpu_cores ? info.cpu_cores + ' core(s)' : '-');
        setText('sys-os-version', info.os_version || '-');

        /* Store RAM budget for layer toggle warnings */
        if (info.ram_budget) ramBudgetMb = parseInt(info.ram_budget) || 0;
        if (info.display_size) {
            var dims = info.display_size.split(',');
            if (dims.length === 2) {
                surfaceMb = Math.ceil(parseInt(dims[0]) * parseInt(dims[1]) * 4 / (1024 * 1024));
            }
        }

        /* Show Pi-Clock OS-only sections */
        if (info.pi_clock_os === 'true') {
            setCardVisibility('os-only-network', true);
            setCardVisibility('os-only-updates', true);
            setCardVisibility('os-only-actions', true);
        }

        /* Show current resolution next to the dropdown */
        var resNote = document.getElementById('res-note');
        if (resNote && info.display_size) {
            resNote.textContent = '(current: ' + info.display_size.replace(',', 'x') + ')';
        }

        /* Limit resolution options based on Pi hardware.
         * Pi 0/1/2/Zero2W: max 1080p (512MB RAM constraint).
         * Pi 3/4/5: full range up to 4K. */
        if (info.max_resolution) {
            var resOrder = ['720p', '1080p', '1440p', '4k'];
            var maxIdx = resOrder.indexOf(info.max_resolution);
            if (maxIdx >= 0) {
                var sel = document.getElementById('display_resolution');
                if (sel) {
                    resOrder.forEach(function(res, i) {
                        var opt = sel.querySelector('option[value="' + res + '"]');
                        if (opt && i > maxIdx) {
                            opt.disabled = true;
                            opt.textContent += ' \u2014 not supported on this hardware';
                            if (sel.value === res) sel.value = info.max_resolution;
                        }
                    });
                }
            }
        }

        /* Show Pi model in system info if available */
        if (info.pi_model) {
            setText('sys-model', info.pi_model);
        }

        /* Re-check RAM budget — layers may have loaded before system
         * info arrived (both are async), so the initial check in
         * loadLayers saw ramBudgetMb=0 and skipped. This second call
         * catches that race. Also runs on the 30s refresh interval. */
        updateRamWarning();

        /* Show or hide persistent reboot card in the toast stack */
        var existing = document.getElementById('reboot-banner');
        if (info.reboot_required === 'true' && info.reboot_reasons) {
            if (!existing) {
                var card = document.createElement('div');
                card.id = 'reboot-banner';
                card.className = 'toast reboot-card';
                var text = document.createElement('div');
                text.className = 'reboot-card-text';
                text.textContent = 'Reboot required: ' + info.reboot_reasons;
                var btn = document.createElement('button');
                btn.type = 'button';
                btn.className = 'btn btn-sm reboot-card-btn';
                btn.textContent = 'Reboot Now';
                btn.addEventListener('click', function() { rebootSystem(); });
                card.appendChild(text);
                card.appendChild(btn);
                getToastStack().appendChild(card);
            } else {
                var textEl = existing.querySelector('.reboot-card-text');
                if (textEl) textEl.textContent =
                    'Reboot required: ' + info.reboot_reasons;
            }
        } else if (existing) {
            existing.remove();
        }
    }).catch(function() {});

    apiCall('GET', '/api/system/slot').then(function(slot) {
        setText('slot-active', 'Slot ' + (slot.ACTIVE_SLOT || '?'));
        setText('slot-a-ver', slot.SLOT_A_VERSION || 'not installed');
        setText('slot-b-ver', slot.SLOT_B_VERSION || 'not installed');

        var pending = slot.PENDING === 'true';
        var badge = document.getElementById('slot-status');
        if (badge) {
            badge.textContent = pending ? 'PENDING' : 'OK';
            badge.className = 'badge badge-' + (pending ? 'warning' : 'success');
        }
    }).catch(function() {});
}

// ── Save display config ─────────────────────────────────────
function saveConfig() {
    var lat = parseFloat(val('qth_lat'));
    var lon = parseFloat(val('qth_lon'));
    if (isNaN(lat) || lat < -90 || lat > 90) {
        toast('Latitude must be between -90 and 90', 'error'); return;
    }
    if (isNaN(lon) || lon < -180 || lon > 180) {
        toast('Longitude must be between -180 and 180', 'error'); return;
    }
    if (lat !== 0 || lon !== 0) {
        document.getElementById('grid_square').value = latLonToGrid(lat, lon);
    }

    var newRes = val('display_resolution');

    apiCall('PUT', '/api/config', {
        center_lon: val('center_lon'),
        qth_lat: val('qth_lat'),
        qth_lon: val('qth_lon'),
        callsign: val('callsign'),
        grid_square: val('grid_square'),
        display_resolution: newRes
    }).then(function() {
        /* Check if resolution changed — needs reboot to take effect */
        var msg = 'Configuration saved';
        var note = document.getElementById('res-note');
        if (note && note.textContent) {
            var current = note.textContent;
            var resMap = {'720p':'1280x720','1080p':'1920x1080','1440p':'2560x1440','4k':'3840x2160'};
            var requested = resMap[newRes] || '';
            if (requested && !current.includes(requested)) {
                msg = 'Saved — reboot required for resolution change';
            }
        }
        toast(msg);
        loadSystemInfo();
    }).catch(function() {
        toast('Save failed', 'error');
    });
}

// ── System overlay (spinner feedback) ───────────────────────
function showOverlay(message) {
    var overlay = document.getElementById('system-overlay');
    var msg = document.getElementById('system-overlay-msg');
    if (overlay && msg) {
        msg.textContent = message;
        overlay.style.display = '';
    }
}

function hideOverlay() {
    var overlay = document.getElementById('system-overlay');
    if (overlay) overlay.style.display = 'none';
    /* Reset upgrade step list and restore spinner for next use */
    var stepsEl = document.getElementById('upgrade-steps');
    if (stepsEl) stepsEl.style.display = 'none';
    var spinner = document.getElementById('overlay-spinner');
    if (spinner) spinner.style.display = '';
}

// ── System actions ──────────────────────────────────────────
function rebootSystem() {
    if (!confirm('Reboot the system?')) return;
    showOverlay('Rebooting — please wait...');
    apiCall('POST', '/api/system/reboot').catch(function() {});
    /* Connection will drop — show the overlay until the page dies */
}

function shutdownSystem() {
    if (!confirm('Shut down the system?\nYou will need physical access to power it back on.')) return;
    showOverlay('Shutting down — safe to remove power in 10 seconds...');
    apiCall('POST', '/api/system/shutdown').catch(function() {});
}

function apkUpgrade() {
    if (!confirm('Update all installed packages?')) return;
    showOverlay('Updating packages — this may take a minute...');
    apiCall('POST', '/api/system/apk-upgrade').then(function(r) {
        hideOverlay();
        toast(r.status || 'Packages updated');
        loadSystemInfo();  /* Show reboot banner immediately */
    }).catch(function() {
        hideOverlay();
        toast('Package update failed', 'error');
    });
}

function osUpgrade() {
    if (!confirm('Download and install a new OS image?\nThe system will need to reboot to activate it.')) return;

    /* Show overlay with step list instead of plain spinner */
    showOverlay('Upgrading OS image...');
    var spinner = document.getElementById('overlay-spinner');
    var stepsEl = document.getElementById('upgrade-steps');
    if (spinner) spinner.style.display = 'none';
    if (stepsEl) {
        stepsEl.style.display = '';
        var allSteps = stepsEl.querySelectorAll('.upgrade-step');
        for (var i = 0; i < allSteps.length; i++) {
            allSteps[i].className = 'upgrade-step pending';
        }
    }

    function setStep(stage) {
        if (!stepsEl) return;
        var steps = stepsEl.querySelectorAll('.upgrade-step');
        var found = false;
        for (var i = 0; i < steps.length; i++) {
            var s = steps[i].getAttribute('data-step');
            if (s === stage) {
                steps[i].className = 'upgrade-step active';
                found = true;
            } else if (!found) {
                steps[i].className = 'upgrade-step done';
            } else {
                steps[i].className = 'upgrade-step pending';
            }
        }
    }

    var receivedDone = false;

    fetch('/api/system/os-upgrade', {
        method: 'POST',
        headers: {'X-Requested-With': 'XMLHttpRequest'},
        credentials: 'same-origin'
    }).then(function(response) {
        if (response.status === 401) {
            window.location.href = '/login';
            return;
        }
        if (!response.ok) {
            finishUpgrade(true, 'Server error (' + response.status + ')');
            return;
        }
        if (!response.body) {
            finishUpgrade(true, 'Streaming not supported by your browser');
            return;
        }
        var reader = response.body.getReader();
        var decoder = new TextDecoder();
        var buf = '';

        function read() {
            reader.read().then(function(result) {
                if (result.done) {
                    /* Stream closed — only treat as success if we got
                     * an explicit STAGE:done event. Otherwise the
                     * connection dropped mid-upgrade. */
                    finishUpgrade(!receivedDone,
                        receivedDone ? null : 'Upgrade interrupted');
                    return;
                }
                buf += decoder.decode(result.value, {stream: true}).replace(/\r\n/g, '\n');
                /* Parse SSE events — each ends with double newline */
                var parts = buf.split('\n\n');
                buf = parts.pop();
                parts.forEach(function(part) {
                    var match = part.match(/^data:\s*(.+)/m);
                    if (!match) return;
                    try {
                        var evt = JSON.parse(match[1]);
                        if (evt.stage === 'done') {
                            receivedDone = true;
                            finishUpgrade(false);
                            return;
                        }
                        if (evt.stage === 'error') {
                            finishUpgrade(true, evt.message);
                            return;
                        }
                        setStep(evt.stage);
                    } catch (e) { /* ignore parse errors */ }
                });
                read();
            }).catch(function() { finishUpgrade(true, 'Connection lost during upgrade'); });
        }
        read();
    }).catch(function() { finishUpgrade(true, 'Failed to connect'); });

    var rebootTimer = null; /* guard against double-fire */
    function finishUpgrade(failed, msg) {
        if (failed) {
            hideOverlay();
            toast(msg || 'OS upgrade failed', 'error');
        } else {
            /* Mark all steps done */
            if (stepsEl) {
                var steps = stepsEl.querySelectorAll('.upgrade-step');
                for (var i = 0; i < steps.length; i++) {
                    steps[i].className = 'upgrade-step done';
                }
            }
            /* Show success and auto-reboot after 5 seconds.
             * No confirm dialog — if they started the upgrade,
             * they want to activate it. The countdown gives them
             * a moment to read the success message. */
            var overlayMsg = document.getElementById('system-overlay-msg');
            var countdown = 5;
            if (overlayMsg) overlayMsg.textContent =
                'OS image installed — rebooting in ' + countdown + 's...';
            if (rebootTimer) return; /* prevent double-fire */
            rebootTimer = setInterval(function() {
                countdown--;
                if (countdown <= 0) {
                    clearInterval(rebootTimer);
                    if (overlayMsg) overlayMsg.textContent = 'Rebooting...';
                    apiCall('POST', '/api/system/reboot').catch(function() {});
                } else if (overlayMsg) {
                    overlayMsg.textContent =
                        'OS image installed — rebooting in ' + countdown + 's...';
                }
            }, 1000);
        }
    }
}

function rollbackSystem() {
    if (!confirm('Roll back to the previous OS version?')) return;
    showOverlay('Rolling back...');
    apiCall('POST', '/api/system/rollback').then(function(r) {
        hideOverlay();
        toast(r.status || 'Rollback complete');
    }).catch(function() {
        hideOverlay();
        toast('Rollback failed', 'error');
    });
}

// ── Layer controls ──────────────────────────────────────────
function loadLayers() {
    return apiCall('GET', '/api/layers').then(function(layers) {
        var container = document.getElementById('layer-controls');
        if (!container) return {};
        container.innerHTML = '';

        var layerState = {};

        /* Layers that are always on — no toggle, just opacity control */
        var requiredLayers = {'basemap': true, 'daylight': true};
        /* Layers with no useful opacity control */
        var noOpacity = {'ticker': true};

        /* Map layer names to their config card IDs — defined once
         * outside the loop to avoid rebuilding on every iteration. */
        var configCards = {
            'dxspots': 'dx-cluster-card',
            'satellites': 'satellite-select-card',
            'ticker': 'ticker-sources-card',
            'bandconditions': 'propagation-card',
            'lightning': 'lightning-card',
            'earthquakes': 'earthquake-card'
        };

        layers.forEach(function(layer) {
            var row = document.createElement('div');
            row.className = 'layer-row';

            var opacityPct = Math.round(layer.opacity * 100);

            if (requiredLayers[layer.name]) {
                /* Required layer — no toggle, just a spacer to keep alignment */
                var spacer = document.createElement('div');
                spacer.className = 'layer-toggle';
                row.appendChild(spacer);
            } else {
                /* Toggle switch */
                var toggle = buildToggle('data-layer', layer.name, layer.enabled);
                toggle.querySelector('input').addEventListener('change', updateRamWarning);
                row.appendChild(toggle);
            }

            /* Layer name */
            row.appendChild(buildLabel(layer.label));

            if (!noOpacity[layer.name]) {
                /* Opacity slider */
                var opDiv = document.createElement('div');
                opDiv.className = 'layer-opacity';
                var slider = document.createElement('input');
                slider.type = 'range';
                slider.min = '0';
                slider.max = '100';
                slider.value = opacityPct;
                slider.setAttribute('data-layer-opacity', layer.name);
                opDiv.appendChild(slider);
                row.appendChild(opDiv);

                /* Opacity value label */
                var opVal = document.createElement('span');
                opVal.className = 'layer-opacity-val';
                opVal.textContent = opacityPct + '%';
                row.appendChild(opVal);
            }

            /* Live slider update — capture opVal in IIFE to avoid
             * var-in-loop closure bug (all handlers would share the
             * last opVal otherwise). */
            (function(label) {
                var sl = row.querySelector('input[type="range"]');
                if (sl) sl.addEventListener('input', function() {
                    label.textContent = this.value + '%';
                });
            })(opVal);

            /* Settings gear column — fixed width so labels line up.
             * Layers with config cards get a clickable gear icon;
             * layers without get an empty spacer of the same width.
             * The gear is hidden when the layer is disabled — otherwise
             * clicking it would scroll to a hidden card and look broken. */
            {
                var col = document.createElement('div');
                col.className = 'layer-settings-col';

                if (configCards[layer.name]) {
                    (function(cardId) {
                        var btn = document.createElement('button');
                        btn.className = 'layer-settings-btn';
                        /* Inline SVG referencing the shared sprite. Using
                         * innerHTML rather than createElementNS keeps the
                         * call site short and matches the static template's
                         * <use href="..."/> style. */
                        btn.innerHTML = '<svg class="icon" aria-hidden="true"><use href="/static/icons.svg#icon-settings"/></svg>';
                        btn.title = 'Settings';
                        btn.setAttribute('aria-label', 'Open layer settings');
                        if (!layer.enabled) btn.style.display = 'none';
                        btn.addEventListener('click', function() {
                            var card = document.getElementById(cardId);
                            if (card) card.scrollIntoView({behavior: 'smooth', block: 'center'});
                        });
                        col.appendChild(btn);
                    })(configCards[layer.name]);
                }

                /* Insert after toggle, before label */
                if (row.children.length >= 1) {
                    row.insertBefore(col, row.children[1]);
                } else {
                    row.appendChild(col);
                }
            }

            container.appendChild(row);
            layerState[layer.name] = layer.enabled;

            /* Link satellite/ticker card visibility to layer toggles */
            var cb = row.querySelector('input[type="checkbox"]');
            if (layer.name === 'dxspots') {
                setCardVisibility('dx-cluster-card', layer.enabled);
                cb.addEventListener('change', function() {
                    setCardVisibility('dx-cluster-card', this.checked);
                    if (this.checked) loadDxSettings();
                });
            }
            if (layer.name === 'satellites') {
                setCardVisibility('satellite-select-card', layer.enabled);
                cb.addEventListener('change', function() {
                    setCardVisibility('satellite-select-card', this.checked);
                    if (this.checked) loadSatellites();
                });
            }
            if (layer.name === 'ticker') {
                setCardVisibility('ticker-sources-card', layer.enabled);
                cb.addEventListener('change', function() {
                    setCardVisibility('ticker-sources-card', this.checked);
                    if (this.checked) loadTickerSources();
                });
            }
            if (layer.name === 'bandconditions') {
                setCardVisibility('propagation-card', layer.enabled);
                cb.addEventListener('change', function() {
                    setCardVisibility('propagation-card', this.checked);
                    if (this.checked) loadPropSettings();
                });
            }
            if (layer.name === 'lightning') {
                setCardVisibility('lightning-card', layer.enabled);
                cb.addEventListener('change', function() {
                    setCardVisibility('lightning-card', this.checked);
                    if (this.checked) loadLightningSettings();
                });
            }
            if (layer.name === 'earthquakes') {
                setCardVisibility('earthquake-card', layer.enabled);
                cb.addEventListener('change', function() {
                    setCardVisibility('earthquake-card', this.checked);
                    if (this.checked) loadEarthquakeSettings();
                });
            }

            /* Generic: keep the gear button in sync with the layer toggle.
             * Every layer that has a settings card also has its gear hidden
             * while disabled (set at build time above). This handler flips
             * it back to visible when the layer is re-enabled. Placed after
             * the per-layer branches so it applies uniformly. */
            if (configCards[layer.name]) {
                cb.addEventListener('change', function() {
                    var b = row.querySelector('.layer-settings-btn');
                    if (b) b.style.display = this.checked ? '' : 'none';
                });
            }
        });

        /* Check RAM budget now that all toggles are built */
        updateRamWarning();

        return layerState;
    }).catch(function() { return {}; });
}

function saveLayers() {
    var layers = [];
    var requiredLayers = {'basemap': true, 'daylight': true};

    /* Collect toggleable layers */
    document.querySelectorAll('[data-layer]').forEach(function(toggle) {
        var name = toggle.getAttribute('data-layer');
        var slider = document.querySelector('[data-layer-opacity="' + name + '"]');
        layers.push({
            name: name,
            enabled: toggle.checked,
            opacity: slider ? parseInt(slider.value) / 100 : 1.0
        });
    });

    /* Always include required layers as enabled */
    document.querySelectorAll('[data-layer-opacity]').forEach(function(slider) {
        var name = slider.getAttribute('data-layer-opacity');
        if (requiredLayers[name]) {
            layers.push({
                name: name,
                enabled: true,
                opacity: parseInt(slider.value) / 100
            });
        }
    });

    apiCall('PUT', '/api/layers', layers).then(function() {
        toast('Layers saved');
        /* The renderer auto-hides the dxfeed applet while the
         * dxspots layer is off, so no dashboard-side sync is
         * needed here — a prior implementation toggled a now-
         * removed checkbox and had been silently dead. If we
         * ever want the Applets board to visibly move the
         * dxfeed card to the Disabled column when the layer
         * flips off, it'll need a DOM-level move-and-save
         * rather than a hidden toggle. */
    }).catch(function() {
        toast('Save failed', 'error');
    });
}

// ── DX Cluster settings ─────────────────────────────────────

/* Band definitions for the DX cluster filter UI.
 * idx = bit position in the 12-bit DX_BANDS bitmask stored in
 * pi-clock-renderer.conf (bit 0 = 160m, bit 11 = 2m). The renderer
 * tests (band_mask & (1u << band_index)) to filter spots.
 * Colours match the renderer's band colour table in dxspot.c. */
var bandDefs = [
    {idx: 0,  name: '160m', color: '#8B0000'},
    {idx: 1,  name: '80m',  color: '#FF4500'},
    {idx: 2,  name: '60m',  color: '#FF8C00'},
    {idx: 3,  name: '40m',  color: '#FFA500'},
    {idx: 4,  name: '30m',  color: '#FFFF00'},
    {idx: 5,  name: '20m',  color: '#00FF00'},
    {idx: 6,  name: '17m',  color: '#00FFFF'},
    {idx: 7,  name: '15m',  color: '#4D80FF'},
    {idx: 8,  name: '12m',  color: '#8A2BE2'},
    {idx: 9,  name: '10m',  color: '#FF00FF'},
    {idx: 10, name: '6m',   color: '#FFFFFF'},
    {idx: 11, name: '2m',   color: '#AAAAAA'},
];

/* Pick black or white text based on background luminance (ITU-R BT.709).
 * Handles both #rrggbb and #rgb formats. */
function bandTextColor(hex) {
    if (hex.length === 4) {
        hex = '#' + hex[1]+hex[1] + hex[2]+hex[2] + hex[3]+hex[3];
    }
    var r = parseInt(hex.substr(1, 2), 16);
    var g = parseInt(hex.substr(3, 2), 16);
    var b = parseInt(hex.substr(5, 2), 16);
    var lum = (0.2126 * r + 0.7152 * g + 0.0722 * b) / 255;
    return lum > 0.5 ? '#000000' : '#ffffff';
}

function loadDxSettings() {
    apiCall('GET', '/api/dx-settings').then(function(settings) {
        var container = document.getElementById('band-toggles');
        if (!container) return;
        container.innerHTML = '';

        /* Parse saved band mask — 12-bit hex, default all bands on */
        var savedMask = parseInt(settings.dx_bands || '0FFF', 16);

        var distEl = document.getElementById('dx_distance');
        if (distEl) distEl.value = settings.dx_distance || '0';
        var ageEl = document.getElementById('dx_spot_age');
        if (ageEl) ageEl.value = settings.dx_spot_age || '900';

        bandDefs.forEach(function(band) {
            var isActive = (savedMask & (1 << band.idx)) !== 0;
            var btn = document.createElement('span');
            btn.className = 'band-btn' + (isActive ? ' active' : '');
            btn.textContent = band.name;
            btn.style.borderColor = band.color;
            btn.setAttribute('data-band-idx', band.idx);

            if (isActive) {
                btn.style.backgroundColor = band.color;
                btn.style.color = bandTextColor(band.color);
            } else {
                btn.style.backgroundColor = 'transparent';
                btn.style.color = 'var(--text-secondary)';
            }

            btn.onclick = function() {
                this.classList.toggle('active');
                if (this.classList.contains('active')) {
                    this.style.backgroundColor = band.color;
                    this.style.color = bandTextColor(band.color);
                } else {
                    this.style.backgroundColor = 'transparent';
                    this.style.color = 'var(--text-secondary)';
                }
            };
            container.appendChild(btn);
        });
    }).catch(function() {});
}

function saveDxSettings() {
    var mask = 0;
    document.querySelectorAll('[data-band-idx]').forEach(function(btn) {
        if (btn.classList.contains('active')) {
            mask |= (1 << parseInt(btn.getAttribute('data-band-idx')));
        }
    });

    var hexMask = mask.toString(16).toUpperCase();
    while (hexMask.length < 4) hexMask = '0' + hexMask;

    apiCall('PUT', '/api/dx-settings', {
        dx_distance: val('dx_distance'),
        dx_bands: hexMask,
        dx_spot_age: val('dx_spot_age')
    }).then(function() {
        toast('DX settings saved');
    }).catch(function() {
        toast('Save failed', 'error');
    });
}

// ── Propagation / Band Conditions settings ──────────────────
/* Propagation band defs — colours match DX cluster exactly */
var propBandDefs = [
    {idx: 0, name: '80m', color: '#FF4500'},  /* same as bandDefs[1] */
    {idx: 1, name: '40m', color: '#FFA500'},  /* same as bandDefs[3] */
    {idx: 2, name: '30m', color: '#FFFF00'},  /* same as bandDefs[4] */
    {idx: 3, name: '20m', color: '#00FF00'},  /* same as bandDefs[5] */
    {idx: 4, name: '17m', color: '#00FFFF'},  /* same as bandDefs[6] */
    {idx: 5, name: '15m', color: '#4D80FF'},  /* same as bandDefs[7] */
    {idx: 6, name: '12m', color: '#8A2BE2'},  /* same as bandDefs[8] */
    {idx: 7, name: '10m', color: '#FF00FF'},  /* same as bandDefs[9] */
];

function loadPropSettings() {
    apiCall('GET', '/api/prop-settings').then(function(settings) {
        var container = document.getElementById('prop-band-toggles');
        if (!container) return;
        container.innerHTML = '';

        var savedMask = parseInt(settings.prop_bands || 'FF', 16);

        /* Antenna gain slider */
        var gainSlider = document.getElementById('antenna_gain');
        var gainLabel = document.getElementById('gain-value');
        if (gainSlider) {
            gainSlider.value = settings.antenna_gain || '0';
            if (gainLabel) gainLabel.textContent = gainSlider.value;
            gainSlider.addEventListener('input', function() {
                if (gainLabel) gainLabel.textContent = this.value;
            });
        }

        propBandDefs.forEach(function(band) {
            var isActive = (savedMask & (1 << band.idx)) !== 0;
            var btn = document.createElement('span');
            btn.className = 'band-btn' + (isActive ? ' active' : '');
            btn.textContent = band.name;
            btn.style.borderColor = band.color;
            btn.setAttribute('data-prop-band-idx', band.idx);

            if (isActive) {
                btn.style.backgroundColor = band.color;
                btn.style.color = bandTextColor(band.color);
            } else {
                btn.style.backgroundColor = 'transparent';
                btn.style.color = 'var(--text-secondary)';
            }

            btn.onclick = function() {
                this.classList.toggle('active');
                if (this.classList.contains('active')) {
                    this.style.backgroundColor = band.color;
                    this.style.color = bandTextColor(band.color);
                } else {
                    this.style.backgroundColor = 'transparent';
                    this.style.color = 'var(--text-secondary)';
                }
            };
            container.appendChild(btn);
        });
    }).catch(function() {});
}

function savePropSettings() {
    var mask = 0;
    document.querySelectorAll('[data-prop-band-idx]').forEach(function(btn) {
        if (btn.classList.contains('active')) {
            mask |= (1 << parseInt(btn.getAttribute('data-prop-band-idx')));
        }
    });

    var hexMask = mask.toString(16).toUpperCase();
    while (hexMask.length < 2) hexMask = '0' + hexMask;

    apiCall('PUT', '/api/prop-settings', {
        prop_bands: hexMask,
        antenna_gain: val('antenna_gain') || '0'
    }).then(function() {
        toast('Propagation settings saved');
    }).catch(function() {
        toast('Save failed', 'error');
    });
}

// ── Lightning settings ──────────────────────────────────────
function loadLightningSettings() {
    apiCall('GET', '/api/lightning-settings').then(function(settings) {
        var sel = document.getElementById('lightning_fade_ms');
        if (sel) sel.value = settings.lightning_fade_ms || '15000';
    }).catch(function() {});
}

function saveLightningSettings() {
    apiCall('PUT', '/api/lightning-settings', {
        lightning_fade_ms: val('lightning_fade_ms') || '15000'
    }).then(function() {
        toast('Lightning settings saved');
    }).catch(function() {
        toast('Save failed', 'error');
    });
}

// ── Earthquake settings ────────────────────────────────────
function loadEarthquakeSettings() {
    apiCall('GET', '/api/earthquake-settings').then(function(settings) {
        var sel = document.getElementById('earthquake_display_s');
        if (sel) sel.value = settings.earthquake_display_s || '86400';
    }).catch(function() {});
}

function saveEarthquakeSettings() {
    apiCall('PUT', '/api/earthquake-settings', {
        earthquake_display_s: val('earthquake_display_s') || '1800'
    }).then(function() {
        toast('Earthquake settings saved');
    }).catch(function() {
        toast('Save failed', 'error');
    });
}

// ── Applet controls ─────────────────────────────────────────
/* ── Applet drag-and-drop board ─────────────────────────────
 * Three droppable columns: Left panel, Right panel, Disabled.
 * Each `.applet-card` is draggable between columns (changes side,
 * or toggles the enabled bit when moved to/from Disabled) and
 * reorderable within a column (changes the visual stacking order
 * on the display).
 *
 * The backend persists the configured applets in the exact order
 * the UI sends them — first-in-array = top of the on-screen panel
 * within each side. saveApplets() walks the two active columns in
 * DOM order (top to bottom) to reconstruct that, then appends
 * disabled cards with their last-known side so the renderer still
 * has a stable "where would this go if re-enabled" answer. */

/* Element currently being dragged — module-scoped because dragover
 * and drop handlers on potential targets need to know which card is
 * in flight. The native DataTransfer object would also work but
 * can't carry a direct element reference across handlers. */
var dragApplet = null;

/* Single reusable drop-position indicator. Lazily created on the
 * first drag, then moved around the DOM during dragover to preview
 * where the card will land. Kept as a module-scoped singleton so
 * we never have more than one indicator visible at a time. */
var dropIndicator = null;

function getDropIndicator() {
    if (!dropIndicator) {
        dropIndicator = document.createElement('div');
        dropIndicator.className = 'applet-drop-indicator';
        dropIndicator.setAttribute('aria-hidden', 'true');
    }
    return dropIndicator;
}

function removeDropIndicator() {
    if (dropIndicator && dropIndicator.parentNode) {
        dropIndicator.parentNode.removeChild(dropIndicator);
    }
}

/* Position the indicator inside `zone` at the slot that would be
 * chosen if the user released at clientY. Uses the same "cursor
 * above the midpoint" rule as onAppletDrop, so preview and drop
 * land on the same slot. Skips the dragged card and the indicator
 * itself when searching, so self-placement and re-entry don't
 * confuse the calculation. */
function updateDropIndicator(zone, clientY) {
    if (!dragApplet) return;
    var ind = getDropIndicator();
    var children = Array.prototype.slice.call(zone.children);
    var insertBefore = null;
    for (var i = 0; i < children.length; i++) {
        var el = children[i];
        if (el === dragApplet || el === ind) continue;
        if (!el.classList || !el.classList.contains('applet-card')) continue;
        var rect = el.getBoundingClientRect();
        if (clientY < rect.top + rect.height / 2) {
            insertBefore = el;
            break;
        }
    }
    if (insertBefore) {
        zone.insertBefore(ind, insertBefore);
    } else {
        zone.appendChild(ind);
    }
}

function loadApplets() {
    apiCall('GET', '/api/applets').then(function(applets) {
        /* Clear every dropzone — the server's response is the
         * source of truth, so a rebuild from scratch avoids stale
         * cards from a prior page state. */
        ['left', 'right', 'disabled'].forEach(function(bucket) {
            var zone = document.querySelector(
                '[data-applet-dropzone="' + bucket + '"]');
            if (zone) zone.innerHTML = '';
        });

        /* Render each applet into its current bucket in the
         * order the server returned it. "disabled" is inferred
         * from the enabled flag, not from a separate field. */
        applets.forEach(function(applet) {
            var bucket = applet.enabled ? applet.side : 'disabled';
            /* Guard against an unexpected side value from an
             * older config file — send it to disabled rather
             * than dropping the card. */
            if (bucket !== 'left' && bucket !== 'right' && bucket !== 'disabled') {
                bucket = 'disabled';
            }
            var zone = document.querySelector(
                '[data-applet-dropzone="' + bucket + '"]');
            if (zone) zone.appendChild(buildAppletCard(applet));
        });

        wireAppletCards();
    }).catch(function() {});
}

/* Attach drop handlers to the three dropzone elements ONCE — they are
 * static HTML that never gets rebuilt. Called from DOMContentLoaded.
 * Kept separate from wireAppletCards so repeated loadApplets() calls
 * (future polling, manual refresh) can't stack duplicate listeners
 * on the zones. */
function initAppletDropzones() {
    document.querySelectorAll('.applet-dropzone').forEach(function(zone) {
        zone.addEventListener('dragover',  onAppletDragOver);
        zone.addEventListener('dragleave', onAppletDragLeave);
        zone.addEventListener('drop',      onAppletDrop);
    });
}

/* Build a single draggable applet card. The card carries its
 * display-independent state (name, label, last side) as data-*
 * attributes so the DOM is the authoritative model — saveApplets()
 * can reconstruct the payload from nothing but the rendered tree. */
function buildAppletCard(applet) {
    var card = document.createElement('div');
    card.className = 'applet-card';
    card.setAttribute('draggable', 'true');
    card.setAttribute('data-applet-name', applet.name);
    /* data-applet-last-side remembers where the applet was before
     * it was dragged to Disabled, so re-enabling (back into Left
     * or Right directly) doesn't need a round-trip to pick a
     * default. Updated on every drop into a non-disabled column. */
    card.setAttribute('data-applet-last-side',
        (applet.side === 'left' || applet.side === 'right') ? applet.side : 'right');

    var grip = document.createElement('span');
    grip.className = 'applet-card-grip';
    grip.setAttribute('aria-hidden', 'true');
    card.appendChild(grip);

    var label = document.createElement('span');
    label.textContent = applet.label;
    card.appendChild(label);

    return card;
}

/* Attach drag handlers to every card. Cards are rebuilt on each
 * loadApplets() call, so listeners ride with the new DOM — old cards
 * are garbage-collected with their listeners. Dropzone listeners are
 * wired separately in initAppletDropzones() because those elements
 * persist across rebuilds. */
function wireAppletCards() {
    document.querySelectorAll('.applet-card').forEach(function(card) {
        card.addEventListener('dragstart', onAppletDragStart);
        card.addEventListener('dragend',   onAppletDragEnd);
    });
}

function onAppletDragStart(e) {
    dragApplet = this;
    this.classList.add('dragging');
    /* Required for Firefox — without setData the drag events
     * don't fire at all. The value is ignored; we use the
     * dragApplet module variable for the actual payload. */
    if (e.dataTransfer) {
        e.dataTransfer.effectAllowed = 'move';
        e.dataTransfer.setData('text/plain', this.getAttribute('data-applet-name') || '');
    }
}

function onAppletDragEnd() {
    this.classList.remove('dragging');
    document.querySelectorAll('.applet-dropzone.drag-over').forEach(function(z) {
        z.classList.remove('drag-over');
    });
    /* Always clear the indicator when the drag ends — drops go
     * through onAppletDrop (which also removes it) but cancelled
     * drags only fire dragend, so we need the cleanup here too. */
    removeDropIndicator();
    dragApplet = null;
}

function onAppletDragOver(e) {
    if (!dragApplet) return;
    e.preventDefault(); /* required so drop will fire */
    if (e.dataTransfer) e.dataTransfer.dropEffect = 'move';
    this.classList.add('drag-over');
    /* Move the indicator to the slot the cursor is currently over.
     * dragover fires continuously during the drag, so this preview
     * follows the pointer in real time. */
    updateDropIndicator(this, e.clientY);
}

function onAppletDragLeave(e) {
    /* dragleave fires when leaving child nodes too — only clear
     * the highlight when we actually leave the zone rectangle.
     * relatedTarget is null when the pointer leaves the window. */
    if (e.relatedTarget && this.contains(e.relatedTarget)) return;
    this.classList.remove('drag-over');
    /* Remove the indicator only if it lives in THIS zone. When the
     * user sweeps the card into another zone, dragover there will
     * move the same indicator element into the new zone before we
     * get here — so we must not remove it from under that zone. */
    if (dropIndicator && dropIndicator.parentNode === this) {
        this.removeChild(dropIndicator);
    }
}

function onAppletDrop(e) {
    e.preventDefault();
    this.classList.remove('drag-over');
    if (!dragApplet) return;

    /* The indicator has been tracking the cursor via dragover, so
     * by the time we get here it already sits in the exact slot
     * the user targeted. Insert the dragged card at that slot,
     * then remove the indicator. */
    var ind = dropIndicator;
    if (ind && ind.parentNode === this) {
        /* No-op guard: if the card is already immediately before
         * the indicator, inserting it there would be a redundant
         * DOM move (and harmless), but we skip it so the browser
         * doesn't flash the card's rendering for no reason. */
        if (dragApplet.nextElementSibling !== ind) {
            this.insertBefore(dragApplet, ind);
        }
        this.removeChild(ind);
    } else if (dragApplet.parentNode !== this) {
        /* Safety net: indicator missing (e.g. drop fired before any
         * dragover, rare but not impossible) and the card is in a
         * different zone. Append to this zone so we never lose the
         * card. onAppletDragEnd will still clean up the indicator. */
        this.appendChild(dragApplet);
    }

    /* If we dropped into Left or Right, remember that as the
     * applet's "last side" so a later move into Disabled still
     * knows the preferred column for re-enablement. */
    var bucket = this.getAttribute('data-applet-dropzone');
    if (bucket === 'left' || bucket === 'right') {
        dragApplet.setAttribute('data-applet-last-side', bucket);
    }
}

/* Serialise the DOM tree into the payload expected by PUT /api/applets.
 * Walks Left, then Right, then Disabled. Order within each column is
 * DOM order (top to bottom), which is exactly the order the renderer
 * will use when stacking the panels. */
function saveApplets() {
    var applets = [];

    function collect(bucket, isEnabled) {
        var zone = document.querySelector('[data-applet-dropzone="' + bucket + '"]');
        if (!zone) return;
        zone.querySelectorAll('.applet-card').forEach(function(card) {
            var name = card.getAttribute('data-applet-name');
            if (!name) return;
            /* Enabled cards use the bucket they were dropped into
             * ("left" or "right"). Disabled cards fall back to
             * data-applet-last-side so a later re-enable lands in
             * the column the user last chose — with a right-side
             * fallback that matches the backend's own default when
             * no prior preference exists. */
            var side = isEnabled
                ? bucket
                : (card.getAttribute('data-applet-last-side') || 'right');
            applets.push({ name: name, enabled: isEnabled, side: side });
        });
    }

    collect('left', true);
    collect('right', true);
    collect('disabled', false);

    apiCall('PUT', '/api/applets', applets).then(function() {
        toast('Panel settings saved');
    }).catch(function(err) {
        toast(err && err.message ? err.message : 'Save failed', 'error');
    });
}

// ── Satellite controls ──────────────────────────────────────
function loadSatellites() {
    apiCall('GET', '/api/satellites').then(function(sats) {
        var container = document.getElementById('satellite-controls');
        if (!container) return;
        container.innerHTML = '';

        sats.forEach(function(sat) {
            container.appendChild(
                buildToggleRow('data-sat', sat.name, sat.label, sat.enabled)
            );
        });
    }).catch(function() {});
}

function saveSatellites() {
    var sats = [];
    document.querySelectorAll('[data-sat]').forEach(function(toggle) {
        sats.push({
            name: toggle.getAttribute('data-sat'),
            enabled: toggle.checked
        });
    });

    apiCall('PUT', '/api/satellites', sats).then(function() {
        toast('Satellite selection saved');
    }).catch(function() {
        toast('Save failed', 'error');
    });
}

// ── Ticker source controls ──────────────────────────────────
function loadTickerSources() {
    apiCall('GET', '/api/ticker-sources').then(function(data) {
        var container = document.getElementById('ticker-controls');
        if (!container) return;
        container.innerHTML = '';

        /* Set mode dropdown */
        var modeSelect = document.getElementById('ticker_mode');
        if (modeSelect && data.mode !== undefined) {
            modeSelect.value = data.mode;
        }

        /* On single-core, disable smooth scroll option */
        apiCall('GET', '/api/system/info').then(function(info) {
            if (info.cpu_cores === '1' && modeSelect) {
                var scrollOpt = modeSelect.querySelector('option[value="0"]');
                if (scrollOpt) {
                    scrollOpt.disabled = true;
                    scrollOpt.textContent = 'Smooth Scroll (multi-core only)';
                }
                /* Force away from scroll if currently selected */
                if (modeSelect.value === '0') modeSelect.value = '1';
            }
        }).catch(function() {});

        (data.sources || []).forEach(function(src) {
            container.appendChild(
                buildToggleRow('data-ticker-src', src.name,
                               src.label, src.enabled)
            );
        });
    }).catch(function() {});
}

function saveTickerSources() {
    var sources = [];
    document.querySelectorAll('[data-ticker-src]').forEach(function(toggle) {
        sources.push({
            name: toggle.getAttribute('data-ticker-src'),
            enabled: toggle.checked
        });
    });

    var mode = parseInt(val('ticker_mode') || '1');

    apiCall('PUT', '/api/ticker-sources', {sources: sources, mode: mode}).then(function() {
        toast('Ticker sources saved');
    }).catch(function() {
        toast('Save failed', 'error');
    });
}

// ── Update check ───────────────────────────────────────────
function checkForUpdates() {
    apiCall('GET', '/api/system/update-check').then(function(data) {
        var el = document.getElementById('sys-update-available');
        if (!el) return;
        if (data.update) {
            el.textContent = data.available + ' (update available)';
            el.style.color = 'var(--warning)';
        } else if (data.available) {
            el.textContent = data.available + ' (up to date)';
            el.style.color = 'var(--success)';
        } else {
            el.textContent = 'unable to check';
        }
    }).catch(function() {
        setText('sys-update-available', 'unable to check');
    });
}

// ── Hostname ───────────────────────────────────────────────
function loadHostname() {
    apiCall('GET', '/api/system/hostname').then(function(data) {
        if (data.hostname) document.getElementById('sys_hostname').value = data.hostname;
    }).catch(function() {});
}

function saveHostname() {
    var h = val('sys_hostname');
    if (!h) { toast('Hostname is required', 'error'); return; }
    if (!/^[a-zA-Z0-9]([a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?$/.test(h)) {
        toast('Invalid hostname (letters, digits, hyphens only)', 'error'); return;
    }

    apiCall('PUT', '/api/system/hostname', {hostname: h}).then(function(data) {
        toast(data.status || 'Hostname saved');
    }).catch(function() {
        toast('Hostname save failed', 'error');
    });
}

// ── WiFi configuration ─────────────────────────────────────
function loadWifi() {
    apiCall('GET', '/api/network/wifi').then(function(wifi) {
        if (wifi.ssid) document.getElementById('wifi_ssid').value = wifi.ssid;
        /* Only select the country if the backend reports one that the
         * dropdown actually lists — otherwise fall through to the
         * <select>'s default ('GB'). Prevents an unlisted value from
         * leaving the control blank and confusing a subsequent save. */
        if (wifi.country) {
            var sel = document.getElementById('wifi_country');
            var match = false;
            for (var i = 0; i < sel.options.length; i++) {
                if (sel.options[i].value === wifi.country) { match = true; break; }
            }
            if (match) sel.value = wifi.country;
        }
        var openBox = document.getElementById('wifi_open');
        if (openBox) {
            openBox.checked = !!wifi.open_network;
            onWifiOpenToggle();
        }
    }).catch(function() {});
}

/* Disable the password field while "Open network" is ticked. The
 * backend ignores the password in open-network mode anyway, but
 * hiding it from input makes the UX match the underlying behaviour
 * and avoids the user believing a typed password took effect. */
function onWifiOpenToggle() {
    var openBox = document.getElementById('wifi_open');
    var pw = document.getElementById('wifi_password');
    if (!openBox || !pw) return;
    pw.disabled = openBox.checked;
    if (openBox.checked) pw.value = '';
    pw.placeholder = openBox.checked
        ? 'Not required for open networks'
        : 'Leave blank to keep current';
}

/* Delay, in milliseconds, between the "config saved" toast and the
 * actual wpa_supplicant reload. Gives the user a beat to register that
 * the save succeeded before their radio reassociates and (if they're
 * editing over WiFi) their session potentially drops. */
var WIFI_APPLY_DELAY_MS = 5000;

function saveWifi() {
    var ssid = val('wifi_ssid');
    if (!ssid) { toast('SSID is required', 'error'); return; }

    var country = (val('wifi_country') || 'GB').toUpperCase();
    if (!/^[A-Z]{2}$/.test(country)) {
        toast('Country code must be two letters (e.g. GB)', 'error'); return;
    }

    var openBox = document.getElementById('wifi_open');
    var openNetwork = !!(openBox && openBox.checked);
    var password = openNetwork ? '' : val('wifi_password');

    /* Fail fast on an obviously-too-short passphrase rather than
     * round-tripping to the server. A blank password is explicitly
     * allowed (keeps the existing PSK when the SSID is unchanged,
     * which the backend enforces); the check only kicks in when the
     * user has typed something. */
    if (password && password.length < 8) {
        toast('WiFi password must be 8-63 characters', 'error'); return;
    }

    apiCall('PUT', '/api/network/wifi', {
        ssid: ssid,
        password: password,
        country: country,
        open_network: openNetwork
    }).then(function() {
        document.getElementById('wifi_password').value = '';
        toast('WiFi configuration saved');
        /* Schedule the apply step. Deliberately detached from the
         * save Promise chain so a failing/slow connect doesn't retro-
         * actively mark the save itself as failed — the config is
         * already on disk either way. */
        setTimeout(applyWifi, WIFI_APPLY_DELAY_MS);
    }).catch(function(err) {
        /* Surface the backend's validation message where possible so
         * users can tell a country/SSID/password validation failure
         * apart from a server-side error. apiCall rethrows with the
         * body.error text when the response is non-2xx. */
        toast(err && err.message ? err.message : 'WiFi save failed', 'error');
    });
}

/* Triggers the second half of the save flow: asks the backend to
 * reload wpa_supplicant and poll until the association state machine
 * reaches COMPLETED (or the server's 20 s budget runs out).
 *
 * Shown as two sequential toasts so the user can tell "applying" apart
 * from the final outcome. Kept exported (window-global via function
 * declaration) so the UI flow is testable without forcing a save. */
function applyWifi() {
    toast('Applying new WiFi settings — this can take up to 20 seconds');

    apiCall('POST', '/api/network/wifi/connect').then(function(result) {
        if (result && result.connected) {
            var ssid = result.ssid || 'the network';
            var ip = result.ip ? ' (' + result.ip + ')' : '';
            toast('Connected to ' + ssid + ip);
        } else {
            /* Non-connected outcome: show the wpa_state so the user
             * knows whether to retry (SCANNING / DISCONNECTED usually
             * means wrong PSK or AP out of range), and remind them the
             * config is persisted either way. */
            var state = (result && result.state) ? result.state : 'unknown';
            toast('Not yet connected (state: ' + state +
                  '). The new config will apply on next boot.', 'error');
        }
    }).catch(function(err) {
        /* Most common reason for this branch: the browser is on the
         * WiFi the Pi just left, so the HTTP response never arrives.
         * Reassure the user that the save itself still took. */
        var msg = (err && err.message) ? err.message : 'connection test failed';
        toast('Could not confirm connection (' + msg +
              '). Config is saved and will apply on next boot.', 'error');
    });
}

// ── Password change ────────────────────────────────────────
function changePassword() {
    var current = val('pw_current');
    var newPw = val('pw_new');
    var confirmPw = val('pw_confirm');

    if (!current || !newPw) { toast('All fields are required', 'error'); return; }
    if (newPw !== confirmPw) { toast('New passwords do not match', 'error'); return; }
    if (newPw.length < 8) { toast('Password too short (min 8 characters)', 'error'); return; }

    apiCall('POST', '/api/system/change-password', {
        current: current,
        'new': newPw
    }).then(function() {
        /* Password changed — force logout so user re-authenticates */
        window.location.href = '/login';
    }).catch(function(err) {
        toast(err.message || 'Password change failed', 'error');
    });
}

// ── Init ────────────────────────────────────────────────────
document.addEventListener('DOMContentLoaded', function() {
    loadSystemInfo();
    checkForUpdates();
    /* Wire dropzone listeners once — before loadApplets() populates
     * cards — since the dropzone elements are static HTML and survive
     * every subsequent loadApplets() rebuild. Card listeners are
     * (re-)attached inside loadApplets() via wireAppletCards(). */
    initAppletDropzones();
    loadApplets();
    loadHostname();
    loadWifi();

    /* Load layers first and wait for the result. The layer toggles
     * control visibility of the DX, satellite, and ticker config cards,
     * so we need to know which layers are enabled before deciding
     * whether to populate those panels. Loading them unconditionally
     * wastes API calls for features the user has disabled. */
    loadLayers().then(function(layerState) {
        if (layerState.dxspots) loadDxSettings();
        if (layerState.bandconditions) loadPropSettings();
        if (layerState.satellites) loadSatellites();
        if (layerState.lightning) loadLightningSettings();
        if (layerState.earthquakes) loadEarthquakeSettings();
        if (layerState.ticker) loadTickerSources();
    });

    setInterval(loadSystemInfo, 30000);
});

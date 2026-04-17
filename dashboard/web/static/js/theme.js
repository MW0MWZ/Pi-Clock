/*
 * theme.js - Light/dark theme toggle
 *
 * Reads theme preference from localStorage, applies it immediately
 * to prevent flash of wrong theme. Toggle button switches between
 * light and dark.
 *
 * Copyright (C) 2026 Pi-Clock Contributors
 * SPDX-License-Identifier: GPL-2.0-only
 */

/*
 * IIFE (Immediately Invoked Function Expression) keeps all variables
 * out of the global scope, preventing conflicts with other scripts.
 * Runs immediately on script load — before DOMContentLoaded — so the
 * theme is applied before the first paint, preventing a flash of the
 * wrong theme.
 */
(function() {
    // Apply saved theme immediately (before DOM renders).
    // We set data-theme on documentElement (<html>) rather than <body>
    // because <body> isn't available yet at script load time, and CSS
    // custom property inheritance starts at the root element.
    var saved = localStorage.getItem('pi-clock-theme') || 'dark';
    document.documentElement.setAttribute('data-theme', saved);

    // Set up toggle button when DOM is ready
    document.addEventListener('DOMContentLoaded', function() {
        var btn = document.getElementById('theme-toggle');
        if (!btn) return;

        updateButton(btn, saved);

        btn.addEventListener('click', function() {
            var current = document.documentElement.getAttribute('data-theme');
            var next = current === 'dark' ? 'light' : 'dark';
            document.documentElement.setAttribute('data-theme', next);
            localStorage.setItem('pi-clock-theme', next);
            updateButton(btn, next);
        });
    });

    /*
     * Reflect the active theme in the toggle button. The button text
     * describes the action (what clicking it will do), so "Light Mode"
     * appears while the dark theme is active, and vice versa. The icon
     * is a <use> reference into the shared sprite; swapping href is
     * cheaper than rebuilding the button DOM on every click.
     */
    function updateButton(btn, theme) {
        var label = btn.querySelector('#theme-label');
        var iconUse = btn.querySelector('#theme-icon-use');
        if (theme === 'dark') {
            if (label) label.textContent = 'Light Mode';
            if (iconUse) iconUse.setAttribute('href', '/static/icons.svg#icon-sun');
        } else {
            if (label) label.textContent = 'Dark Mode';
            if (iconUse) iconUse.setAttribute('href', '/static/icons.svg#icon-moon');
        }
    }
})();

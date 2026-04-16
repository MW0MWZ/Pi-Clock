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

    function updateButton(btn, theme) {
        btn.textContent = theme === 'dark' ? '\u2600 Light Mode' : '\u263E Dark Mode';
    }
})();

/*
 * callsign_districts.c - Callsign prefix to geographic location table
 *
 * Maps callsign district prefixes to approximate geographic centres
 * for countries where the callsign number/prefix encodes a region.
 * Used to place DX spots more accurately on the map — without this,
 * all US stations pin to Missouri, all Canadian to Ontario, etc.
 *
 * The table is checked by pic_ctydat_lookup_location() when the
 * primary cty.dat prefix match does not have a <lat/lon> override.
 * Longest-prefix-match: "W1ABC" matches "W1" (New England) before
 * "W" (generic USA).
 *
 * Sources: ARRL callsign district maps, RAC prefix allocations,
 * ACMA/Ofcom/JARL district definitions. Coordinates are approximate
 * geographic centres of the covered region.
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "callsign_districts.h"
#include <string.h>
#include <ctype.h>

/*
 * District override table — sorted by prefix for readability.
 * The lookup function does a longest-prefix-match scan, so order
 * does not affect correctness. Keep entries grouped by country
 * for maintainability.
 */
static const pic_district_t districts[] = {
    /* ── United States (W/K/N/AA-AL) ──────────────────────────
     * US callsign number = district. W1=New England, W6=California etc.
     * K and N prefixes follow the same numbering. */
    {"W1",   42.5,  -71.5},   /* New England (CT,MA,ME,NH,RI,VT) */
    {"W2",   41.0,  -74.5},   /* New York, New Jersey */
    {"W3",   40.5,  -77.0},   /* Pennsylvania, Delaware, DC, Maryland */
    {"W4",   34.0,  -82.0},   /* Southeast (AL,FL,GA,KY,NC,SC,TN,VA) */
    {"W5",   33.0,  -96.0},   /* South-central (AR,LA,MS,NM,OK,TX) */
    {"W6",   37.0, -120.0},   /* California */
    {"W7",   42.0, -114.0},   /* Northwest (AZ,ID,MT,NV,OR,UT,WA,WY) */
    {"W8",   41.0,  -83.0},   /* Michigan, Ohio, West Virginia */
    {"W9",   41.5,  -88.0},   /* Illinois, Indiana, Wisconsin */
    {"W0",   42.5,  -98.0},   /* Midwest (CO,IA,KS,MN,MO,NE,ND,SD) */
    {"K1",   42.5,  -71.5},   /* Same districts for K prefix */
    {"K2",   41.0,  -74.5},
    {"K3",   40.5,  -77.0},
    {"K4",   34.0,  -82.0},
    {"K5",   33.0,  -96.0},
    {"K6",   37.0, -120.0},
    {"K7",   42.0, -114.0},
    {"K8",   41.0,  -83.0},
    {"K9",   41.5,  -88.0},
    {"K0",   42.5,  -98.0},
    {"N1",   42.5,  -71.5},   /* Same districts for N prefix */
    {"N2",   41.0,  -74.5},
    {"N3",   40.5,  -77.0},
    {"N4",   34.0,  -82.0},
    {"N5",   33.0,  -96.0},
    {"N6",   37.0, -120.0},
    {"N7",   42.0, -114.0},
    {"N8",   41.0,  -83.0},
    {"N9",   41.5,  -88.0},
    {"N0",   42.5,  -98.0},
    {"AA1",  42.5,  -71.5},   /* Extended prefixes (AA-AL) */
    {"AA2",  41.0,  -74.5},
    {"AA3",  40.5,  -77.0},
    {"AA4",  34.0,  -82.0},
    {"AA5",  33.0,  -96.0},
    {"AA6",  37.0, -120.0},
    {"AA7",  42.0, -114.0},
    {"AA8",  41.0,  -83.0},
    {"AA9",  41.5,  -88.0},
    {"AA0",  42.5,  -98.0},

    /* ── Canada (VE/VA) ───────────────────────────────────────
     * VE number = province. VA is the secondary block. */
    {"VE1",  45.1,  -63.3},   /* Nova Scotia */
    {"VE2",  53.4,  -71.8},   /* Quebec */
    {"VE3",  50.4,  -86.0},   /* Ontario */
    {"VE4",  54.9,  -97.4},   /* Manitoba */
    {"VE5",  54.4, -105.9},   /* Saskatchewan */
    {"VE6",  55.2, -114.5},   /* Alberta */
    {"VE7",  54.8, -124.8},   /* British Columbia */
    {"VE8",  66.4, -119.0},   /* Northwest Territories */
    {"VE9",  46.6,  -66.4},   /* New Brunswick */
    {"VA1",  45.1,  -63.3},   /* Nova Scotia (VA block) */
    {"VA2",  53.4,  -71.8},   /* Quebec */
    {"VA3",  50.4,  -86.0},   /* Ontario */
    {"VA4",  54.9,  -97.4},   /* Manitoba */
    {"VA5",  54.4, -105.9},   /* Saskatchewan */
    {"VA6",  55.2, -114.5},   /* Alberta */
    {"VA7",  54.8, -124.8},   /* British Columbia */
    {"VO1",  48.5,  -56.1},   /* Newfoundland */
    {"VO2",  54.0,  -62.0},   /* Labrador */
    {"VY0",  71.0,  -88.9},   /* Nunavut */
    {"VY1",  63.6, -135.5},   /* Yukon */
    {"VY2",  46.4,  -63.2},   /* Prince Edward Island */

    /* ── Australia (VK) ───────────────────────────────────────
     * VK number = state/territory. */
    {"VK1",  -35.5, 149.0},   /* Australian Capital Territory */
    {"VK2",  -32.2, 147.0},   /* New South Wales */
    {"VK3",  -36.9, 144.3},   /* Victoria */
    {"VK4",  -22.5, 144.4},   /* Queensland */
    {"VK5",  -30.1, 135.8},   /* South Australia */
    {"VK6",  -25.3, 122.3},   /* Western Australia */
    {"VK7",  -42.0, 146.6},   /* Tasmania */
    {"VK8",  -19.5, 132.6},   /* Northern Territory */

    /* ── Russia — European (UA/RA) ────────────────────────────
     * District number gives rough region within European Russia. */
    {"UA1",  59.9,   30.3},   /* Northwest (St. Petersburg) */
    {"UA3",  55.8,   37.6},   /* Central (Moscow) */
    {"UA4",  53.2,   50.2},   /* Volga (Samara/Kazan) */
    {"UA6",  45.0,   39.0},   /* South/Caucasus (Krasnodar) */
    {"RA1",  59.9,   30.3},
    {"RA3",  55.8,   37.6},
    {"RA4",  53.2,   50.2},
    {"RA6",  45.0,   39.0},
    {"R1",   59.9,   30.3},   /* Modern R prefix */
    {"R3",   55.8,   37.6},
    {"R4",   53.2,   50.2},
    {"R6",   45.0,   39.0},

    /* ── Russia — Asiatic (UA9/UA0/RA9/RA0) ───────────────── */
    {"UA9",  56.8,   60.6},   /* Urals/West Siberia */
    {"UA0",  56.0,   92.9},   /* East Siberia/Far East */
    {"RA9",  56.8,   60.6},
    {"RA0",  56.0,   92.9},
    {"R9",   56.8,   60.6},
    {"R0",   56.0,   92.9},

    /* ── Japan (JA/JH/JR etc.) ────────────────────────────────
     * Number = region. All J-prefix variants follow same numbering. */
    {"JA1",  35.7,  139.7},   /* Kanto (Tokyo) */
    {"JA2",  35.2,  136.9},   /* Tokai (Nagoya) */
    {"JA3",  34.7,  135.5},   /* Kinki (Osaka) */
    {"JA4",  34.4,  132.5},   /* Chugoku (Hiroshima) */
    {"JA5",  34.3,  134.1},   /* Shikoku */
    {"JA6",  33.6,  130.4},   /* Kyushu (Fukuoka) */
    {"JA7",  38.3,  140.9},   /* Tohoku (Sendai) */
    {"JA8",  43.1,  141.4},   /* Hokkaido (Sapporo) */
    {"JA9",  36.6,  136.7},   /* Hokuriku (Kanazawa) */
    {"JA0",  36.7,  138.2},   /* Shin'etsu (Nagano) */
    {"JH1",  35.7,  139.7},   /* JH prefix — same districts */
    {"JH2",  35.2,  136.9},
    {"JH3",  34.7,  135.5},
    {"JH4",  34.4,  132.5},
    {"JH5",  34.3,  134.1},
    {"JH6",  33.6,  130.4},
    {"JH7",  38.3,  140.9},
    {"JH8",  43.1,  141.4},
    {"JR1",  35.7,  139.7},   /* JR prefix */
    {"JR2",  35.2,  136.9},
    {"JR3",  34.7,  135.5},
    {"JR6",  33.6,  130.4},
    {"JR8",  43.1,  141.4},

    /* ── Brazil (PY/PU/PP/PR/PS/PT) ──────────────────────────
     * Number = region. Multiple prefix letters share numbering. */
    {"PY1",  -21.0, -42.0},   /* Rio de Janeiro, Espírito Santo */
    {"PY2",  -20.0, -48.0},   /* São Paulo */
    {"PY3",  -30.0, -53.0},   /* Rio Grande do Sul */
    {"PY4",  -18.5, -44.5},   /* Minas Gerais */
    {"PY5",  -25.5, -50.5},   /* Paraná, Santa Catarina */
    {"PY6",  -12.0, -41.0},   /* Bahia, Sergipe */
    {"PY7",   -7.5, -38.0},   /* Northeast (PE,AL,PB,RN,CE) */
    {"PY8",   -5.0, -55.0},   /* North (Amazonas, Pará) */
    {"PY9",  -16.0, -53.0},   /* Central-West (GO,MT,MS) */
    {"PU1",  -21.0, -42.0},   /* PU prefix — same districts */
    {"PU2",  -20.0, -48.0},
    {"PU3",  -30.0, -53.0},
    {"PU5",  -25.5, -50.5},

    /* ── Italy (I/IK/IZ/IW/IU) ───────────────────────────────
     * Number = region. */
    {"I1",   44.5,    8.0},   /* Piedmont, Liguria */
    {"I2",   45.5,    9.7},   /* Lombardy */
    {"I3",   45.8,   11.9},   /* Veneto, Trentino, Friuli */
    {"I4",   44.5,   11.3},   /* Emilia-Romagna */
    {"I5",   43.5,   11.0},   /* Tuscany */
    {"I6",   42.5,   14.0},   /* Marche, Abruzzo */
    {"I7",   40.8,   16.5},   /* Puglia, Basilicata */
    {"I8",   40.2,   15.8},   /* Campania, Calabria */
    {"I0",   41.9,   12.7},   /* Lazio, Umbria */
    {"IK1",  44.5,    8.0},   /* IK prefix — same districts */
    {"IK2",  45.5,    9.7},
    {"IK3",  45.8,   11.9},
    {"IK4",  44.5,   11.3},
    {"IK5",  43.5,   11.0},
    {"IK7",  40.8,   16.5},
    {"IZ1",  44.5,    8.0},   /* IZ prefix */
    {"IZ2",  45.5,    9.7},
    {"IZ3",  45.8,   11.9},
    {"IZ5",  43.5,   11.0},
    {"IZ8",  40.2,   15.8},

    /* ── Spain (EA/EB) ────────────────────────────────────────
     * Number = region (mainland only — islands are separate DXCC). */
    {"EA1",  42.5,   -5.5},   /* Galicia, Asturias, Cantabria */
    {"EA2",  43.0,   -2.0},   /* Basque Country, Navarre */
    {"EA3",  41.8,    1.5},   /* Catalonia */
    {"EA4",  40.0,   -4.5},   /* Madrid, Extremadura */
    {"EA5",  39.5,   -0.5},   /* Valencia, Murcia */
    {"EA7",  37.5,   -5.0},   /* Andalusia */
    {"EB1",  42.5,   -5.5},
    {"EB3",  41.8,    1.5},
    {"EB5",  39.5,   -0.5},

    /* ── Germany (DL/DK/DJ/DH/DF) ────────────────────────────
     * German callsign numbers encode licence CLASS not geography,
     * so we cannot map districts. But the prefix letter after D
     * can hint at region for some older allocations. Not included
     * here — too unreliable for geographic mapping. */

    {NULL, 0, 0}  /* Sentinel */
};

/*
 * pic_district_lookup - Find a district-level coordinate override.
 *
 * Performs longest-prefix-match against the static district table.
 * Returns 1 and fills lat/lon if a match is found, 0 otherwise.
 */
int pic_district_lookup(const char *callsign, double *out_lat, double *out_lon)
{
    char upper[32];
    int len, i;
    const pic_district_t *best = NULL;
    int best_len = 0;

    if (!callsign) return 0;

    /* Uppercase and strip /P /M etc. */
    for (i = 0; callsign[i] && i < 31; i++) {
        if (callsign[i] == '/') break;
        upper[i] = toupper((unsigned char)callsign[i]);
    }
    upper[i] = '\0';
    len = i;

    /* Find the longest matching district prefix */
    for (i = 0; districts[i].prefix; i++) {
        int plen = strlen(districts[i].prefix);
        if (plen > len) continue;
        if (plen > best_len && strncmp(upper, districts[i].prefix, plen) == 0) {
            best = &districts[i];
            best_len = plen;
        }
    }

    if (best) {
        if (out_lat) *out_lat = best->lat;
        if (out_lon) *out_lon = best->lon;
        return 1;
    }
    return 0;
}

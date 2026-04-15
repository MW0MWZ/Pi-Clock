/*
 * dxcluster.h - DX cluster telnet client
 *
 * Connects to a DX cluster server in a background thread, receives
 * spot lines, parses them, resolves callsign locations via cty.dat,
 * and adds them to the spot list for display.
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef PIC_DXCLUSTER_H
#define PIC_DXCLUSTER_H

#include "dxspot.h"
#include "ctydat.h"

/*
 * pic_dxcluster_start - Start the DX cluster client thread.
 *
 * Connects to the specified DX cluster server and begins
 * receiving spots. Spots are added to the provided spot list
 * which the renderer reads during each frame.
 *
 *   host     - Cluster server hostname (e.g., "dxc.ve7cc.net")
 *   port     - Server port (e.g., 7373)
 *   callsign - Your callsign for identification on connect
 *   spots    - Spot list to add received spots to
 *   db       - Country database for callsign resolution
 *   home_prefix - Primary prefix of user's home country (e.g., "G")
 *
 * Returns 0 on success (thread started), -1 on failure.
 */
int pic_dxcluster_start(const char *host, int port,
                        const char *callsign,
                        pic_spotlist_t *spots,
                        pic_ctydat_t *db,
                        const char *home_prefix,
                        double qth_lat, double qth_lon);

/*
 * pic_dxcluster_stop - Stop the DX cluster client thread.
 */
void pic_dxcluster_stop(void);

/*
 * pic_dxcluster_reload - Re-read DX filter settings from config.
 *
 * Called when the reload trigger fires. Updates distance filter,
 * band mask, and spot age. Also expires spots on newly disabled bands.
 */
void pic_dxcluster_reload(pic_spotlist_t *spots);

#endif /* PIC_DXCLUSTER_H */

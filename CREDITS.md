# Credits and Acknowledgements

Pi-Clock is built on the work of many individuals and organisations.
This file credits all third-party data sources, algorithms, libraries,
and datasets used in the project.

## Third-Party Libraries

**stb_image v2.30** — Sean Barrett (nothings.org)
Single-header JPEG/PNG image loader. Public domain.
https://github.com/nothings/stb

## Algorithms and References

**Solar Position** — Jean Meeus, *Astronomical Algorithms* (2nd ed., 1998)
Used for computing solar declination, Greenwich Hour Angle, and the
day/night terminator. Simplified "low accuracy" algorithm accurate to
within ~1 arcminute.

**Lunar Position** — Jean Meeus, *Astronomical Algorithms* (2nd ed., 1998)
Main terms from Chapter 47 for lunar ecliptic longitude, latitude,
and distance. Used for moon position marker and phase calculation.

**HF Propagation Prediction (foF2)** — MINIMUF 3.5
McNamara, L.F., "Prediction of Total Electron Content and Maximum
Usable Frequency Using MINIMUF," DREO Technical Note 82-10, 1982.
Simplified closed-form MUF predictor for the F2 layer critical frequency.

**foF2 Solar Zenith Dependence** — Bradley, P.A. and Dudeney, J.R. (1973)
Empirical cos(chi)^0.6 exponent for interpolating foF2 between
daytime and nighttime values.

**D-Layer Absorption** — ITU-R Recommendation P.533-14
Simplified non-deviative absorption model for calculating the Lowest
Usable Frequency (LUF). Absorption constant K and cos(chi)^1.3
exponent from the ITU-R annex.

**Obliquity Factor (M)** — CCIR Ionospheric Tables
M(3000)F2 values used to calibrate the effective virtual height
parameter (490 km) for the flat-Earth secant formula.

**SGP4 Orbit Propagation** — Vallado, D.A. et al.
Simplified General Perturbations model for predicting satellite
positions from Two-Line Element sets. Implementation follows the
standard SGP4 algorithm simplified for amateur satellite tracking.

## Data Sources

**NOAA Space Weather Prediction Center (SWPC)**
Solar Flux Index, Kp index, solar wind speed, interplanetary magnetic
field (Bz), and X-ray flare classifications. Free JSON API, no
authentication required.
https://services.swpc.noaa.gov

**NASA Solar Dynamics Observatory (SDO)**
Solar disc images in multiple wavelengths (AIA 171Å, 304Å, 193Å,
211/193/171 composite, HMI Intensitygram). Public domain.
https://sdo.gsfc.nasa.gov

**Celestrak** — Dr. T.S. Kelso
Two-Line Element sets for amateur radio satellites. Used for orbit
propagation in the satellite tracking layer.
https://celestrak.org

**AMSAT**
Bulk TLE downloads for amateur satellites (single request instead
of per-satellite queries).
https://www.amsat.org

**AD1C cty.dat** — Jim Reisert, AD1C
Callsign prefix to DXCC entity mapping database. Used for resolving
DX cluster spot locations from callsign prefixes. The "Big CTY"
format includes per-prefix latitude/longitude overrides.
https://www.country-files.com

**HB9HIL CQ Zone Boundaries**
GeoJSON boundary data for CQ zones, used in the CQ zone overlay
layer. Digitised and maintained by HB9HIL.

**Natural Earth Data** — Tom Patterson, US National Park Service
Country border polygons (110m resolution) and timezone boundary
polygons (10m resolution). Public domain.
https://www.naturalearthdata.com

**DX Cluster Network**
Live DX spots received via telnet protocol from public DX Spider
and AR Cluster servers.

**Blitzortung.org** — Community Lightning Detection Network
Real-time lightning strike data received via WebSocket. Global
coverage from volunteer-operated detection stations. Used for the
lightning strikes overlay layer.
https://www.blitzortung.org

**NOAA SWPC OVATION Aurora Model**
Aurora oval probability nowcast from the Space Weather Prediction
Center. JSON grid fetched directly by the Pi every 15 minutes.
Used for the aurora overlay layer.
https://services.swpc.noaa.gov

**NOAA Global Forecast System (GFS)**
Global weather data from the NCEP Global Forecast System. 1-degree
global grid, updated every 6 hours via NOMADS. Pre-processed by a
GitHub Action and served as a compact binary. Fields used:
- UGRD/VGRD: surface wind (10m) for the wind streamline layer
- TCDC: total cloud cover for the cloud cover layer
- APCP: accumulated precipitation for the rain radar layer
https://nomads.ncep.noaa.gov

**USGS Earthquake Hazards Program**
Real-time earthquake data (M4.5+ globally) via GeoJSON feed.
Public domain, US federal government data. Used for the earthquake
overlay layer.
https://earthquake.usgs.gov

**News Feeds**
- NOAA SWPC space weather alerts
- DX World (dxworld.net) — DX news
- ARRL News (arrl.org)
- RSGB GB2RS News (rsgb.org)
- Southgate Amateur Radio Club (southgatearc.org)

## Map Imagery

All maps are **public domain** and free to use, modify, and redistribute.

**NASA Blue Marble Next Generation (BMNG)**
NASA Earth Observatory, data from MODIS instrument on Terra satellite.
21600 x 10800 pixels. July composite for Northern Hemisphere summer vegetation.
https://visibleearth.nasa.gov/collection/1484/blue-marble-next-generation

**NASA Black Marble — Earth at Night**
NASA Earth Observatory / Suomi NPP satellite VIIRS instrument.
NOAA National Geophysical Data Center. 13500 x 6750 pixels.
Composite of Earth's city lights, used for the nighttime hemisphere.
https://visibleearth.nasa.gov/images/79765/earth-at-night

**Natural Earth I — Raster with Shaded Relief and Water**
Cartography by Tom Patterson, US National Park Service. Public domain.
Soft painted style with natural land colours. 16200 x 8100 pixels.
https://www.naturalearthdata.com/downloads/10m-raster-data/10m-natural-earth-1/

**Natural Earth II — Raster with Shaded Relief and Water**
Cartography by Tom Patterson. Warmer, more vibrant palette. 16200 x 8100 pixels.
https://www.naturalearthdata.com/downloads/10m-raster-data/10m-natural-earth-2/

**Cross-Blended Hypsometric Tints**
Cartography by Tom Patterson. Classic atlas-style terrain colouring. 16200 x 8100 pixels.
https://www.naturalearthdata.com/downloads/10m-raster-data/10m-cross-blended-hypso/

**Natural Earth Shaded Relief (Greyscale)**
Cartography by Tom Patterson. Pure greyscale terrain, dark theme base. 16200 x 8100 pixels.
https://www.naturalearthdata.com/downloads/10m-raster-data/10m-shaded-relief/

All maps are resized to 4K, 1440p, 1080p, and 720p using Lanczos resampling.
Output format is JPEG at 95% quality.

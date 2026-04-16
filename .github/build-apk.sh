#!/bin/sh
# build-apk.sh - Build Pi-Clock APK packages
#
# Expects /build to contain: renderer/, packages/, os/
# Expects /keys to contain the signing key
# Outputs APKs to /apk-output
#
# Set SKIP_DASHBOARD=1 to skip the dashboard package.
# Version comes from PKG_VERSION env var.
set -e

APK_KEY_NAME="${APK_KEY_NAME:-pi-clock-apk-signing.rsa}"
PKG_VERSION="${PKG_VERSION:-0.0.1}"

echo "=== Building Pi-Clock APKs v${PKG_VERSION} ==="

# Install build dependencies and cache the APKINDEX
# (abuild needs the cached index to resolve package dependencies)
# python3/imagemagick/curl only needed if REBUILD_MAPS=1
apk update
apk add \
  build-base cairo-dev sdl2-dev openssl-dev pkgconf \
  openssl abuild sudo

# Maps tools needed if rebuilding OR if no cached maps APK exists on gh-pages
EXISTING_MAPS_CHECK=$(find /gh-pages -name "pi-clock-maps-*.apk" 2>/dev/null | head -1)
if [ "$REBUILD_MAPS" = "1" ] || [ -z "$EXISTING_MAPS_CHECK" ]; then
  apk add python3 curl imagemagick libjpeg-turbo
fi

# Setup builder user and signing
adduser -D builder 2>/dev/null || true
addgroup builder abuild 2>/dev/null || true
echo "builder ALL=(ALL) NOPASSWD: ALL" >> /etc/sudoers

mkdir -p /home/builder/.abuild /etc/apk/keys
cp /keys/${APK_KEY_NAME} /home/builder/.abuild/${APK_KEY_NAME}
openssl rsa -in /home/builder/.abuild/${APK_KEY_NAME} \
  -pubout -out /home/builder/.abuild/${APK_KEY_NAME}.pub 2>/dev/null
cp /home/builder/.abuild/${APK_KEY_NAME}.pub /etc/apk/keys/
echo "PACKAGER_PRIVKEY=\"/home/builder/.abuild/${APK_KEY_NAME}\"" \
  > /home/builder/.abuild/abuild.conf
chown -R builder:builder /home/builder

# ── Build renderer ──────────────────────────────────────────
echo "=== Compiling renderer ==="
cp -r /build/renderer /home/builder/renderer-src
cd /home/builder/renderer-src
make clean all VERSION="${PKG_VERSION}"
cd /

# ── Copy pre-built geodata ──────────────────────────────────
# These files are pre-generated and committed to the repo.
# No need for Python, numpy, timezonefinder, or network downloads
# at build time — saves minutes and eliminates dependencies.
echo "=== Copying pre-built geodata ==="
cp /build/renderer/geodata/*.dat /home/builder/

# ── Maps package ──────────────────────────────────────────
# The maps package contains static NASA imagery that never changes.
# If the maps APK already exists in the gh-pages repo at the current
# version, we skip the expensive download/resize step entirely.
# To force a maps rebuild, set REBUILD_MAPS=1.
MAPS_APK="pi-clock-maps-${PKG_VERSION}-r0.apk"
EXISTING_MAPS=$(find /gh-pages -name "pi-clock-maps-*.apk" 2>/dev/null | head -1)

if [ -n "$EXISTING_MAPS" ] && [ "$REBUILD_MAPS" != "1" ]; then
  echo "=== Maps APK exists, keeping as-is: $(basename "$EXISTING_MAPS") ==="
  # Don't copy to apk-output — the deploy step will keep the existing
  # maps APK on gh-pages untouched. Only new/rebuilt APKs go to output.
  SKIP_MAPS_BUILD=1
else
  echo "=== Building maps package (downloading NASA imagery) ==="
  curl -sL --retry 3 --retry-delay 10 --max-time 300 -o /home/builder/bm_src.jpg \
    "https://neo.gsfc.nasa.gov/archive/bluemarble/bmng/world_2km/world.topo.bathy.200407.3x21600x10800.jpg"
  curl -sL --retry 3 --retry-delay 10 --max-time 300 -o /home/builder/km_src.jpg \
    "https://eoimages.gsfc.nasa.gov/images/imagerecords/79000/79765/dnb_land_ocean_ice.2012.13500x6750.jpg"

  for spec in "3840x2160:4k" "2560x1440:1440p" "1920x1080:1080p" "1280x720:720p"; do
    res="${spec%%:*}"; name="${spec##*:}"
    mkdir -p /home/builder/blue_marble /home/builder/black_marble
    echo "  Resizing to $name..."
    convert /home/builder/bm_src.jpg -filter Lanczos -resize "${res}!" \
      -strip -quality 95 "/home/builder/blue_marble/blue_marble_${name}.jpg"
    convert /home/builder/km_src.jpg -filter Lanczos -resize "${res}!" \
      -strip -quality 95 "/home/builder/black_marble/black_marble_${name}.jpg"
  done
  rm -f /home/builder/bm_src.jpg /home/builder/km_src.jpg

  # ── Package: pi-clock-maps ──────────────────────────────
  echo "=== Packaging: pi-clock-maps ==="
  cp -r /build/packages/pi-clock-maps /home/builder/pkg-maps
  sed -i "s/^pkgver=.*/pkgver=${PKG_VERSION}/" /home/builder/pkg-maps/APKBUILD

  cat > /home/builder/pkg-maps/APKBUILD << MEOF
# Maintainer: MW0MWZ <andy@mw0mwz.co.uk>
pkgname=pi-clock-maps
pkgver=${PKG_VERSION}
pkgrel=0
pkgdesc="Pi-Clock map images and geodata"
url="https://github.com/MW0MWZ/Pi-Clock"
arch="noarch"
license="custom:public-domain"
source=""
options="!check"

package() {
    DEST="\$pkgdir/usr/share/pi-clock/maps"
    mkdir -p "\$DEST/blue_marble" "\$DEST/black_marble"
    cp /home/builder/blue_marble/*.jpg "\$DEST/blue_marble/"
    cp /home/builder/black_marble/*.jpg "\$DEST/black_marble/"
    cp /home/builder/borders.dat /home/builder/timezones.dat \
       /home/builder/cqzones.dat /home/builder/ituzones.dat \
       /home/builder/cty.dat \
       /home/builder/tzgrid.dat "\$DEST/"
    cp /home/builder/renderer-src/assets/splash.jpg "\$DEST/" 2>/dev/null || true
}
MEOF
  SKIP_MAPS_BUILD=0
fi

chown -R builder:builder /home/builder
if [ "$SKIP_MAPS_BUILD" != "1" ]; then
  su - builder -c "cd /home/builder/pkg-maps && abuild -r -d"
fi

# ── Package: pi-clock-renderer ──────────────────────────
echo "=== Packaging: pi-clock-renderer ==="
cp -r /build/packages/pi-clock-renderer /home/builder/pkg-renderer
cp /build/os/overlay/etc/init.d/pi-clock-renderer \
   /home/builder/pkg-renderer/pi-clock-renderer.initd
cp /build/os/overlay/etc/init.d/pi-clock-splash \
   /home/builder/pkg-renderer/pi-clock-splash.initd

cat > /home/builder/pkg-renderer/APKBUILD << REOF
# Maintainer: MW0MWZ <andy@mw0mwz.co.uk>
pkgname=pi-clock-renderer
pkgver=${PKG_VERSION}
pkgrel=0
pkgdesc="Pi-Clock world map display renderer"
url="https://github.com/MW0MWZ/Pi-Clock"
arch="armhf"
license="GPL-2.0-only"
depends="pi-clock-maps cairo sdl2 mesa-dri-gallium mesa-egl libdrm ttf-dejavu libssl3 libcrypto3"
install="\$pkgname.post-install \$pkgname.post-upgrade"
source=""
options="!check"

package() {
    install -Dm755 /home/builder/renderer-src/pi-clock \
        "\$pkgdir"/usr/bin/pi-clock
    install -Dm755 /home/builder/renderer-src/pi-clock-splash \
        "\$pkgdir"/usr/bin/pi-clock-splash
    install -Dm755 "\$startdir"/pi-clock-renderer.initd \
        "\$pkgdir"/etc/init.d/pi-clock-renderer
    install -Dm755 "\$startdir"/pi-clock-splash.initd \
        "\$pkgdir"/etc/init.d/pi-clock-splash
    install -dm755 "\$pkgdir"/data/cache
}
REOF

chown -R builder:builder /home/builder/pkg-renderer
# -d skips dependency verification — pi-clock-maps is a runtime dep
# that isn't in Alpine's repos (it's in our custom repo)
su - builder -c "cd /home/builder/pkg-renderer && abuild -r -d"

# ── Copy APKs to output ────────────────────────────────────
echo "=== Copying APKs ==="
find /home/builder/packages -name "*.apk" -exec cp {} /apk-output/ \;
ls -lh /apk-output/
echo "=== Done ==="

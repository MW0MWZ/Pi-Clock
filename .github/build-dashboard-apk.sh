#!/bin/sh
# build-dashboard-apk.sh - Build the Pi-Clock dashboard APK
# Run inside an Alpine Docker container.
set -ex

APK_KEY_NAME="pi-clock-apk-signing.rsa"
PKG_VERSION="${PKG_VERSION:-0.0.0}"
echo "Building dashboard APK version: ${PKG_VERSION}"

# Install build dependencies
apk add --no-cache \
  go build-base openssl abuild sudo

# Create builder user
adduser -D builder
addgroup builder abuild
mkdir -p /etc/sudoers.d
echo "builder ALL=(ALL) NOPASSWD: ALL" > /etc/sudoers.d/builder

# Set up signing key
cp /keys/${APK_KEY_NAME} /tmp/${APK_KEY_NAME}
chmod 644 /tmp/${APK_KEY_NAME}
mkdir -p /etc/apk/keys
openssl rsa -in /tmp/${APK_KEY_NAME} \
  -pubout -out /tmp/${APK_KEY_NAME}.pub 2>/dev/null
cp /tmp/${APK_KEY_NAME}.pub /etc/apk/keys/

# Configure abuild
mkdir -p /home/builder/.abuild
echo "PACKAGER_PRIVKEY=/tmp/${APK_KEY_NAME}" > /home/builder/.abuild/abuild.conf
chown -R builder:builder /home/builder

# Build the dashboard binary
cp -r /src/dashboard /tmp/dashboard
cd /tmp/dashboard
CGO_ENABLED=0 go build \
  -ldflags="-s -w -X main.version=${PKG_VERSION}" \
  -o /tmp/pi-clock-dashboard .

echo "Dashboard binary built: $(ls -lh /tmp/pi-clock-dashboard)"

# Make artifacts readable by builder
chmod a+r /tmp/pi-clock-dashboard

# Create APKBUILD
WORK="/home/builder/apkwork"
mkdir -p "$WORK"
cat > "$WORK/APKBUILD" <<EOF
pkgname=pi-clock-dashboard
pkgver=${PKG_VERSION}
pkgrel=0
pkgdesc="Pi-Clock web management dashboard"
url="https://github.com/MW0MWZ/Pi-Clock"
arch="armhf"
license="GPL-2.0-only"
depends=""
options="!check"
source=""

package() {
    install -Dm755 /tmp/pi-clock-dashboard \
        "\$pkgdir/usr/bin/pi-clock-dashboard"
}
EOF
chown -R builder:builder "$WORK"

# Build the APK
su - builder -c "cd /home/builder/apkwork && abuild -r -d -P /tmp/packages"

# Copy to output
find /tmp/packages -name "*.apk" -exec cp {} /apk-output/ \;

echo "=== Dashboard APK built ==="
ls -lh /apk-output/pi-clock-dashboard*

#!/bin/sh
# index-apk.sh - Generate signed APKINDEX.tar.gz for the APK repository
# Run inside an Alpine Docker container with /keys and /gh-pages mounted.
#
# Usage:
#   docker run ... -v keys:/keys:ro -v gh-pages:/gh-pages \
#     alpine:3.23 sh /gh-pages/../../index-apk.sh <alpine-version>

set -ex

APK_KEY_NAME="pi-clock-apk-signing.rsa"
ALPINE_VERSION="${1:-3.23}"
REPO_DIR="/gh-pages/v${ALPINE_VERSION}/pi-clock/armhf"

cd "$REPO_DIR"

# Generate unsigned APKINDEX from all .apk files
apk index -o APKINDEX.unsigned.tar.gz *.apk

# Sign the APKINDEX with our private key
openssl dgst -sha1 -sign "/keys/${APK_KEY_NAME}" \
  -out ".SIGN.RSA.${APK_KEY_NAME}.pub" \
  APKINDEX.unsigned.tar.gz

# Create signed APKINDEX.tar.gz (signature prepended)
tar -c ".SIGN.RSA.${APK_KEY_NAME}.pub" | \
  cat - APKINDEX.unsigned.tar.gz > APKINDEX.tar.gz

# Clean up temporary files
rm -f APKINDEX.unsigned.tar.gz ".SIGN.RSA.${APK_KEY_NAME}.pub"

echo "=== APKINDEX.tar.gz created and signed ==="
ls -lh "$REPO_DIR"

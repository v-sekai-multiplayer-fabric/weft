#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Generates the self-signed WebTransport TLS cert/key zone-server-h2o
# needs, in the exact shape zone.ex's cert_hash pinning scheme requires:
# an ECDSA P-256 key, at most 14 days of validity. This mirrors
# zone-backend/multiplayer-fabric-generate-secrets/generate-secrets.sh's
# own openssl invocation exactly (same curve, same -days 14, same
# -nodes), fetched and read directly from that script rather than
# guessed, since it is the cert this org's Uro/zone-server pairing
# already trusts clients to pin against.
#
# Usage:
#   ./scripts/generate-tls-cert.sh [common-name] [subject-alt-names]
#
#   common-name          default: zone-server
#   subject-alt-names    default: IP:127.0.0.1,DNS:localhost
#                         for a real deployment, e.g.:
#                         DNS:zone.chibifire.com
#
# Prints the cert_hash (base64 SHA-256 of the DER-encoded cert) zone.ex
# expects in POST /shards's cert_hash field -- computed the same way
# WebTransport's serverCertificateHashes pinning computes it: SHA-256
# over the DER bytes, not the PEM text.

set -e

CN="${1:-zone-server}"
SAN="${2:-IP:127.0.0.1,DNS:localhost}"

CERT_DIR="$(dirname "$0")/../certs"
CERT="$CERT_DIR/zone-server.crt"
KEY="$CERT_DIR/zone-server.key"
mkdir -p "$CERT_DIR"

if [ -f "$CERT" ]; then
  echo "zone-server-h2o: $CERT already exists, not overwriting" >&2
  echo "  (delete it first to regenerate with a new CN/SAN)" >&2
else
  openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 \
    -keyout "$KEY" -out "$CERT" -days 14 -nodes \
    -subj "/CN=$CN" \
    -addext "subjectAltName=$SAN" 2>/dev/null
  chmod 600 "$KEY"
  echo "zone-server-h2o: generated $CERT / $KEY (14-day WebTransport cert, CN=$CN)" >&2
fi

CERT_HASH=$(openssl x509 -in "$CERT" -outform DER | openssl dgst -sha256 -binary | openssl base64 -A)

echo "cert:      $CERT"
echo "key:       $KEY"
echo "cert_hash: $CERT_HASH"

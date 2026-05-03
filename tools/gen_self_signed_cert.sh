#!/usr/bin/env bash
set -euo pipefail

# Generates a local self-signed cert for the mock WSS server.
# Output:
#   tools/cert.pem
#   tools/key.pem

openssl req -x509 -newkey rsa:2048 -nodes \
  -keyout tools/key.pem \
  -out tools/cert.pem \
  -days 365 \
  -subj "/CN=localhost"

echo "Wrote tools/cert.pem and tools/key.pem"

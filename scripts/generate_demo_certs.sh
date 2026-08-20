#!/usr/bin/env bash
# Phase 8 (mTLS): generates a throwaway CA + a domain-server cert + a
# gateway-client cert for the two-tier mTLS demo (config/domain_body_mtls.yaml
# + config/gateway_mtls.yaml). Self-signed, demo-only -- these are exactly
# the "own private CA, not a public one" certs ProxyTarget's tls_ca_cert
# comment describes. Not committed (certs/ is gitignored): regenerate with
# this script instead of shipping keys in the repo.
set -euo pipefail
cd "$(dirname "$0")/.."

OUT=certs
mkdir -p "$OUT"
cd "$OUT"

DAYS=3650

# CA
openssl req -x509 -newkey rsa:2048 -sha256 -days "$DAYS" -nodes \
    -keyout ca.key -out ca.crt -subj "/CN=sovd-toolkit-demo-ca" 2>/dev/null

# Domain server cert (CN=localhost, SAN covers 127.0.0.1 for curl/httplib's
# hostname check against the cert presented on the TLS handshake)
openssl req -newkey rsa:2048 -nodes -keyout domain.key -out domain.csr \
    -subj "/CN=localhost" 2>/dev/null
openssl x509 -req -in domain.csr -CA ca.crt -CAkey ca.key -CAcreateserial \
    -out domain.crt -days "$DAYS" -sha256 \
    -extfile <(printf "subjectAltName=DNS:localhost,IP:127.0.0.1") 2>/dev/null

# Gateway client cert (CN identifies the caller -- what the domain server's
# mTLS check is actually verifying identity against)
openssl req -newkey rsa:2048 -nodes -keyout gateway_client.key -out gateway_client.csr \
    -subj "/CN=sovd-gateway" 2>/dev/null
openssl x509 -req -in gateway_client.csr -CA ca.crt -CAkey ca.key -CAcreateserial \
    -out gateway_client.crt -days "$DAYS" -sha256 2>/dev/null

rm -f domain.csr gateway_client.csr

echo "Demo certs written to $OUT/:"
ls -1 *.crt *.key

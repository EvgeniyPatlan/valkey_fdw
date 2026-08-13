#!/usr/bin/env sh
#
# Generate the TLS material used by the tls topology.
#
# Produces four identities so that both the positive and the negative
# verification paths are testable:
#
#   ca.crt              the trust root
#   server.crt/key      valid, CN/SAN = valkey
#   expired.crt/key     valid CN, notAfter in the past
#   wronghost.crt/key   valid dates, CN/SAN = not-valkey
#
# Certificates are minted at image build time with fixed validity windows, so
# the expired one is genuinely expired rather than relying on clock skew.
set -eu

OUT="${1:-/tls}"
mkdir -p "$OUT"
cd "$OUT"

subj() { echo "/C=US/ST=Test/L=Test/O=valkey_fdw/CN=$1"; }

# --- CA -------------------------------------------------------------------
openssl genrsa -out ca.key 2048 2>/dev/null
openssl req -x509 -new -nodes -key ca.key -sha256 -days 3650 \
    -subj "$(subj valkey_fdw-test-ca)" -out ca.crt 2>/dev/null

issue() {
    name="$1"; cn="$2"; days="$3"; backdate="$4"

    openssl genrsa -out "${name}.key" 2048 2>/dev/null
    openssl req -new -key "${name}.key" -subj "$(subj "$cn")" -out "${name}.csr" 2>/dev/null

    cat > "${name}.ext" <<EXT
subjectAltName = DNS:${cn}, DNS:localhost, IP:127.0.0.1
extendedKeyUsage = serverAuth, clientAuth
EXT

    if [ "$backdate" = "yes" ]; then
        # Window entirely in the past: notBefore -60d, notAfter -30d.
        openssl x509 -req -in "${name}.csr" -CA ca.crt -CAkey ca.key \
            -CAcreateserial -out "${name}.crt" -sha256 \
            -extfile "${name}.ext" \
            -not_before "$(date -u -d '60 days ago' '+%Y%m%d%H%M%SZ' 2>/dev/null \
                           || date -u -v-60d '+%Y%m%d%H%M%SZ')" \
            -not_after  "$(date -u -d '30 days ago' '+%Y%m%d%H%M%SZ' 2>/dev/null \
                           || date -u -v-30d '+%Y%m%d%H%M%SZ')" 2>/dev/null
    else
        openssl x509 -req -in "${name}.csr" -CA ca.crt -CAkey ca.key \
            -CAcreateserial -out "${name}.crt" -days "$days" -sha256 \
            -extfile "${name}.ext" 2>/dev/null
    fi

    rm -f "${name}.csr" "${name}.ext"
}

issue server    valkey     3650 no
issue wronghost not-valkey 3650 no
issue expired   valkey-expired 0 yes
issue client    client     3650 no

chmod 0644 ./*.crt
chmod 0640 ./*.key
chmod a+r ./*.key   # test material only; never a real secret

echo "TLS material written to $OUT"

#!/bin/sh

external_bins() {
    cat <<'EOF'
curl https://github.com/moparisthebest/static-curl/releases/latest/download/curl-amd64
EOF
}

#!/bin/sh
set -e

# Create data directories on first start in case the bind-mount on the host
# is empty or not yet initialised
mkdir -p /data/paste /data/log

DOMAIN="${FICHE_DOMAIN:-localhost}"
OUTPUT="${FICHE_OUTPUT:-/data/paste}"
EXTRA="${FICHE_ARGS:-}"

exec fiche -d "${DOMAIN}" -o "${OUTPUT}" ${EXTRA}

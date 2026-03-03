#!/bin/sh
# tools/rootfs/stage_bins.sh
#
# Stages coreutils + preferred shell from the host into $COREUTILS_DIR.
# Called by the $(COREUTILS_STAMP) make recipe — keeps all logic out of make.
#
# Required env vars:
#   COREUTILS_DIR   — destination directory
#   HOST_CORE_BINS  — space-separated list of binary names to copy via `which`
#   HOST_SHELL      — absolute path to preferred shell (copied as "sh")

set -e

echo ">>> Staging coreutils + shell from host..."
mkdir -p "${COREUTILS_DIR}"

for bin in ${HOST_CORE_BINS}; do
    p=$(which "${bin}" 2>/dev/null) || continue
    cp "${p}" "${COREUTILS_DIR}/${bin}"
    chmod +x "${COREUTILS_DIR}/${bin}"
done

if [ -n "${HOST_SHELL}" ] && [ -f "${HOST_SHELL}" ]; then
    cp "${HOST_SHELL}" "${COREUTILS_DIR}/sh"
    chmod +x "${COREUTILS_DIR}/sh"
else
    echo "!!! warning: no bash/dash found on host; /bin/sh will be missing"
fi

touch "${COREUTILS_DIR}/.stamp"
echo ">>> Coreutils staging complete."

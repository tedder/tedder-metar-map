#!/bin/bash
# Upload the app image to the beta OTA path unless S3 already has identical bytes
# (single-part S3 ETag = MD5). Runs on every build via the ota_upload target.
# Uses AWS_PROFILE / credentials from the environment. If TMM_ELF_ARCHIVE is set, the ELF is
# kept there as <version>.elf (needed to decode crash addresses from that version).
# usage: ota_upload.sh <bin> <s3 url> [<elf> <version>]
set -euo pipefail
bin=$1 dest=$2 elf=${3:-} ver=${4:-}
bucket_key=${dest#s3://}
bucket=${bucket_key%%/*} key=${bucket_key#*/}
local_md5=$(md5 -q "$bin" 2>/dev/null || md5sum "$bin" | cut -d' ' -f1)
remote_md5=$(aws s3api head-object --bucket "$bucket" --key "$key" \
    --query ETag --output text 2>/dev/null | tr -d '"' || true)
if [ "$local_md5" = "$remote_md5" ]; then
    echo "OTA upload: S3 already has this image ($local_md5), skipped"
    exit 0
fi
aws s3 cp "$bin" "$dest" --cache-control "max-age=30" --only-show-errors
echo "OTA upload: $(basename "$bin") ($local_md5) -> $dest"
if [ -n "${TMM_ELF_ARCHIVE:-}" ] && [ -n "$elf" ] && [ -n "$ver" ]; then
    mkdir -p "$TMM_ELF_ARCHIVE"
    cp "$elf" "$TMM_ELF_ARCHIVE/tedder-metar-map-$ver.elf"
    echo "ELF kept: $TMM_ELF_ARCHIVE/tedder-metar-map-$ver.elf"
fi

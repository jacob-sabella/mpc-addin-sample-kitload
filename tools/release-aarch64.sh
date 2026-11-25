#!/usr/bin/env bash
# The release zip: the build (run ./build-aarch64.sh first) packaged with mpc-vst-plugins' shared addin installer, then
# checked as the catalog will check it. Needs mpc-vst-plugins (open-mpc) at ../../open-mpc (or MPC_VST=/path).
# Usage: tools/release-aarch64.sh <version>   ->  dist/<Name>-<version>-mpc-aarch64.zip
set -euo pipefail
cd "$(dirname "$0")/.."
[ $# -eq 1 ] || { echo "usage: $0 <version>" >&2; exit 2; }
MPC_VST="${MPC_VST:-../../open-mpc}"
python3 "$MPC_VST/tools/release_addin.py" --dir build/aarch64 --version "$1" --repo jacob-sabella/mpc-addin-sample-kitload --license MIT \
  --depends "sample-draw-handler>=1.1.1" \
  --requires "MPC Sample firmware 1.4.0 (on any other firmware it stays off and Load Kit is unchanged)" \
  --about "Load Kit on the MPC Sample, bank by bank: put a kit's banks onto the banks you choose and keep the rest of your pads." -o dist
python3 "$MPC_VST/tools/catalog_check.py" dist/*-"$1"-mpc-aarch64.zip --catalog --expect-id sample-kitload --expect-repo jacob-sabella/mpc-addin-sample-kitload

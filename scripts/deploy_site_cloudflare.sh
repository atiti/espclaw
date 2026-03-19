#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROJECT_NAME="${PROJECT_NAME:-espclaw}"
PRIMARY_DOMAIN="${PRIMARY_DOMAIN:-espclaw.dev}"
BRANCH_NAME="${BRANCH_NAME:-main}"

cd "$ROOT"

./scripts/build_site_wasm.sh

privateinfractl pages ensure-deploy site \
  --project-name="$PROJECT_NAME" \
  --custom-domain="$PRIMARY_DOMAIN" \
  --branch="$BRANCH_NAME" \
  --approve

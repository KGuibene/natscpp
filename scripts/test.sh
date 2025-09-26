#!/usr/bin/env bash
set -euo pipefail

# Start NATS
docker run -d --name nats -p 4222:4222 nats:latest >/dev/null
trap 'docker rm -f nats >/dev/null 2>&1 || true' EXIT

# Wait a bit
sleep 1

# Build & test
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=ON
cmake --build build -j
ctest --test-dir build --output-on-failure

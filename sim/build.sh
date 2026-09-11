#!/bin/bash
# Builds the PC simulator. build.py does the actual work.
#
# 🚨 It used to call gcc directly from here. On Windows, MSYS's fork emulation
#    collided with security software and died, so it moved to Python (09-09).
#    This shell wrapper stays so the callers do not have to change.
set -e
cd "$(dirname "$0")"
exec "${PYTHON:-python3}" build.py "$@"

#!/bin/sh
# Shared limits for the core gates. Sourced by check-cores.sh (guards the build) and
# check-payload.sh (guards the package). One definition, because the same number lived in three
# files and raising it meant finding all three (Codex review 2026-08-14).
#
# 100KB. The smallest genuine artifact today is miyoomini mednafen_vb at 138,104 bytes (tg5040:
# 171,184); the stubs a raced link produces are ~10KB. That margin is only ~1.35x, so a legitimately
# smaller core means editing THIS line, not adding a special case at a call site.
CORE_MIN_SIZE=102400

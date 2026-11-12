#!/usr/bin/env bash
# Build libosmem.so, build the test snippets and run the full suite.
# Under macOS/Apple Silicon this uses Docker (the suite needs Linux
# `ltrace`); under native Linux it just calls `make check-fast`.

set -euo pipefail
cd "$(dirname "$0")"

IMAGE=osmem-arm64

run_native() {
	( cd src   && make )
	( cd tests && make clean_snippets && make snippets && python3 run_tests.py -d )
}

run_docker() {
	if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
		echo "==> building Docker image ($IMAGE)"
		docker build -t "$IMAGE" .
	fi
	docker run --rm -v "$PWD":/w -w /w "$IMAGE" bash -c '
		set -e
		# On aarch64 ltrace prefixes libcalls with the caller binary
		# ("test-foo->os_malloc(...)") and requires -l libosmem.so to
		# emit anything useful. Patch run_tests.py in place; on
		# x86_64 these changes are no-ops.
		python3 - <<"PY"
import pathlib
p = pathlib.Path("tests/run_tests.py")
s = p.read_text()
s = s.replace("\"-x\",\n                \"os_*\",",
              "\"-l\",\n                \"libosmem.so\",")
needle = "linker_brk = Call(raw_lines[0])"
patch  = ("raw_lines = [re.sub(r\"(^\\\\s*)[\\\\w.-]+->\", r\"\\\\1\", l) "
          "for l in raw_lines]\n        " + needle)
p.write_text(s.replace(needle, patch, 1))
PY
		echo "==> building libosmem.so"
		( cd src   && make clean >/dev/null 2>&1 && make >/dev/null 2>&1 )
		echo "==> building test snippets"
		( cd tests && make clean_snippets >/dev/null 2>&1 && make snippets >/dev/null 2>&1 )
		echo "==> running test suite"
		( cd tests && python3 run_tests.py -d )
	'
}

case "$(uname -s)" in
	Linux)	run_native ;;
	*)	run_docker ;;
esac

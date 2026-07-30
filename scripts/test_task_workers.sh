#!/usr/bin/env bash
# Native-only TASK worker regression entrypoint.  It never touches a live service.
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
"$repo/scripts/test_task_queue_native.sh"
# Keep the parent-supervision translation unit buildable independently of the
# Wasmtime object; runtime acceptance is run by the deployment harness that has
# the WASI SDK/Wasmtime toolchain and executes this same isolated fixture.
compiler=$(command -v clang++ || command -v g++)
id=$(sha256sum "$repo"/src/capy/*.{cpp,h} "$repo"/src/lib/compiler.cpp "$repo"/src/lib/compiler-parser.cpp | sha256sum | awk '{print $1}')
tmp=${TMPDIR:-/tmp}/capy-task-workers-main.$$.o
trap 'rm -f "$tmp"' EXIT
"$compiler" -c "$repo/src/linux_fastcgi.cpp" -D'EXEC_NAME="bearer_fastcgi"' -D'PLATFORM_NAME="linux"' -std=c++20 -fpermissive -w "-DCAPY_COMPILER_BUILD_ID=\"$id\"" -o "$tmp"
python3 - "$repo/src/linux_fastcgi.cpp" <<'PY'
import re
import sys
source = open(sys.argv[1], encoding="utf-8").read()
cancel = re.search(r"task_queue::Result cancel\(const String& id\)\n\{(.*?)\n\}", source, re.S)
assert cancel, "TASK cancel seam missing"
assert "task_queue::cancel" in cancel.group(1), "request cancellation must persist through queue"
assert "task_worker_processes" not in cancel.group(1) and "kill(" not in cancel.group(1), "request worker must not signal inherited worker state"
assert "cancellation_requests" in source and "SYS_pidfd_send_signal" in source, "parent must read leases and use pinned child identity"
assert "worker_unavailable" in source and "admission_healthy" in source, "admission must fail closed while the parent is unhealthy"
assert "execution_deadline" in source and "wasm_invocation_remaining_ms(execution_deadline)" in source, "compile and handler must share deadline"
PY
echo "task worker transport and native supervisor compile passed"

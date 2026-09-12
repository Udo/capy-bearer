#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parent.parent

def text(path: str) -> str:
    return (root / path).read_text()

for builder in ("scripts/make_deb.sh", "scripts/make_rpm.sh"):
    source = text(builder)
    assert 'cp -a "$REPO_ROOT/site/."' not in source, builder
    assert 'for path in LICENSE README.md codesearch scripts src docs' not in source, builder
    assert 'scripts/bearer-cli' in source, builder
    assert 'scripts/systemd/wait-ready.sh' in source, builder
    assert 'cp -a "$REPO_ROOT/etc/bearer"' not in source, builder
    assert 'libwasmtime.so*' in source, builder
    assert 'cp -a "$resolved"' not in source, builder
    assert "_WEBROOT" not in source, builder
    assert "_INCLUDE_TESTS" not in source, builder
    assert "/usr/lib/bearer/site(" in source, builder
    assert '"page_runtime_error=site/errors/runtime-error.capy": "page_runtime_error="' in source, builder

assert 'du -sk --apparent-size "$STAGE_DIR"' in text("scripts/make_deb.sh")

for unit in ("scripts/deb/bearer.service", "scripts/systemd/bearer.service"):
    source = text(unit)
    assert "User=www-data\n" in source, unit
    assert "Group=www-data\n" in source, unit
    assert "MemoryMax=" not in source, unit
    assert "StateDirectory=bearer\n" in source, unit
    assert "CacheDirectory=bearer\n" in source, unit

rpm_service = text("scripts/rpm/bearer.service")
assert "User=bearer\n" in rpm_service
assert "Group=bearer\n" in rpm_service
assert "MemoryMax=" not in rpm_service
rpm_socket = text("scripts/rpm/bearer.socket")
assert "SocketUser=bearer\n" in rpm_socket
assert "SocketGroup=bearer\n" in rpm_socket
assert "SocketMode=0660\n" in rpm_socket
rpm_builder = text("scripts/make_rpm.sh")
assert 'useradd --system --gid bearer' in rpm_builder
assert 'scripts/rpm/bearer.service' in rpm_builder
assert "chown -R bearer:bearer /var/cache/bearer /var/lib/bearer" in rpm_builder

postinst = text("scripts/deb/postinst")
assert "page_runtime_error=site/errors/runtime-error.capy" in postinst
assert "chown -R www-data:www-data /var/cache/bearer /var/lib/bearer" in postinst
assert ".www-data-owner" in postinst

control = text("scripts/deb/control.in")
assert "published site" not in control
assert "test tree" not in control
assert "Applications and public web roots are packaged separately." in control

settings = text("etc/bearer/settings.cfg")
assert "WORKER_COUNT=4\n" in settings
assert "PROACTIVE_COMPILE_JOBS=2\n" in settings
assert "page_runtime_error=site/errors/runtime-error.capy\n" in settings

setup = text("docs/setup.md")
assert "The package does not create or populate a public webroot." in setup
assert "MemoryMax=2G" in setup
assert "Independent applications need separate Bearer instances" in setup

print("runtime package layout: ok")

#!/usr/bin/env python3

import hashlib
import os
import sys
import tarfile
import tempfile
import urllib.request


def die(message: str) -> None:
  print(f"fetch_codex: {message}", file=sys.stderr)
  raise SystemExit(1)


def main() -> None:
  if len(sys.argv) != 5:
    die("usage: fetch_codex.py URL SHA256 MEMBER OUTPUT")

  url, expected_sha256, member_name, output = sys.argv[1:]
  output = os.path.abspath(output)
  os.makedirs(os.path.dirname(output), exist_ok=True)

  with tempfile.TemporaryDirectory(prefix="spore-codex-") as tmp:
    archive = os.path.join(tmp, "codex.tar.gz")
    print(f"fetch_codex: downloading {url}")
    try:
      urllib.request.urlretrieve(url, archive)
    except Exception as exc:
      die(f"download failed: {exc}")

    digest = hashlib.sha256()
    with open(archive, "rb") as file:
      for chunk in iter(lambda: file.read(1024 * 1024), b""):
        digest.update(chunk)
    actual_sha256 = digest.hexdigest()
    if actual_sha256 != expected_sha256:
      die(
        "sha256 mismatch: "
        f"expected {expected_sha256}, got {actual_sha256}"
      )

    try:
      with tarfile.open(archive, "r:gz") as tar:
        member = tar.getmember(member_name)
        source = tar.extractfile(member)
        if source is None:
          die(f"archive member is not a regular file: {member_name}")
        with open(output, "wb") as dest:
          while chunk := source.read(1024 * 1024):
            dest.write(chunk)
    except (KeyError, tarfile.TarError) as exc:
      die(f"failed to extract {member_name}: {exc}")

  os.chmod(output, 0o755)
  print(f"fetch_codex: wrote {output}")


if __name__ == "__main__":
  main()

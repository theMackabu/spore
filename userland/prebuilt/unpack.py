"""Install selected files from a pinned guest package into the build directory."""

import hashlib
import io
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
import zipfile


def publish(path, data, mode):
    tmp = tempfile.NamedTemporaryFile(dir=path.parent, delete=False)
    try:
        with tmp:
            tmp.write(data)
            os.fchmod(tmp.fileno(), mode)
        os.replace(tmp.name, path)
    finally:
        Path(tmp.name).unlink(missing_ok=True)


def fetch(cache, url, digest):
    cache.mkdir(parents=True, exist_ok=True)
    archive = cache / digest
    if archive.is_file():
        data = archive.read_bytes()
        if hashlib.sha256(data).hexdigest() == digest:
            return data
    with urllib.request.urlopen(url, timeout=60) as response:
        data = response.read()
    if hashlib.sha256(data).hexdigest() != digest:
        raise ValueError(f"SHA-256 mismatch for {url}")
    publish(archive, data, 0o644)
    return data


def install(cache, url, digest, dest, members):
    data = io.BytesIO(fetch(cache, url, digest))
    if url.endswith('.zip'):
        with zipfile.ZipFile(data) as archive:
            for member in members:
                publish(dest / Path(member).name, archive.read(member), 0o755)
    else:
        mode = 'r:gz'
        if url.endswith('.xbps'):
            tar = subprocess.run(['zstd', '-dc'], input=data.getvalue(), stdout=subprocess.PIPE, check=True)
            data = io.BytesIO(tar.stdout)
            mode = 'r:'
        # APK v2 concatenates signature, metadata and data tar streams.
        with tarfile.open(fileobj=data, mode=mode, ignore_zeros=True) as archive:
            for member in members:
                info = archive.getmember(member)
                if not info.isfile():
                    raise ValueError(f"Expected regular file {member} in {url}")
                with archive.extractfile(info) as src:
                    publish(dest / Path(member).name, src.read(), info.mode & 0o777)


if __name__ == '__main__':
    cache, url, digest, dest, *members = sys.argv[1:]
    install(Path(cache), url, digest, Path(dest), members)

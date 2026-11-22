"""Stage the target headers, static libraries and pkg-config files for a port."""

import io
from pathlib import Path
import shutil
import sys
import tarfile
import tempfile

from unpack import fetch, publish


def install(cache, dest, stamp, archives):
    with tempfile.TemporaryDirectory(dir=dest.parent) as tmp:
        root = Path(tmp) / 'sysroot'
        for url, digest in zip(archives[::2], archives[1::2], strict=True):
            data = fetch(cache, url, digest)
            with tarfile.open(fileobj=io.BytesIO(data), mode='r:gz', ignore_zeros=True) as package:
                for member in package:
                    path = Path(member.name)
                    if not member.isfile() or '..' in path.parts:
                        continue
                    if not (path.is_relative_to('usr/include') or (path.parent == Path('usr/lib') and path.suffix == '.a')
                            or (path.parent == Path('usr/lib/pkgconfig') and path.suffix == '.pc')):
                        continue
                    out = root / path
                    out.parent.mkdir(parents=True, exist_ok=True)
                    with package.extractfile(member) as src:
                        out.write_bytes(src.read())
        if dest.exists():
            shutil.rmtree(dest)
        root.rename(dest)
    publish(stamp, ('\n'.join(archives[1::2]) + '\n').encode(), 0o644)


if __name__ == '__main__':
    cache, dest, stamp, *archives = sys.argv[1:]
    install(Path(cache), Path(dest), Path(stamp), archives)

"""Print tab-separated target arguments for Meson at configure time."""

import json
from pathlib import Path
import sys


def rows(manifest, section):
    if section == 'packages':
        for package in manifest['packages']:
            yield [package['url'], package['sha256'], *package['files']]
    elif section == 'sdks':
        for name, archives in manifest['sdks'].items():
            fields = [name]
            for archive in archives:
                fields.extend([archive['url'], archive['sha256']])
            yield fields
    else:
        raise ValueError(f'Unknown manifest section: {section}')


if __name__ == '__main__':
    path, section = sys.argv[1:]
    for fields in rows(json.loads(Path(path).read_text()), section):
        print('\t'.join(fields))

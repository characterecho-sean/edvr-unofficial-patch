"""Fetch the pinned official Khronos Windows x64 OpenXR loader.

python tools/fetch_openxr_loader.py [--archive package.nupkg] [--out directory]
python tools/fetch_openxr_loader.py --verify [--out directory]

Only the pinned DLL is read from the archive; archive paths are never extracted.
The build verifies the DLL and its notices offline and never chooses a loader
from a headset vendor or SteamVR installation.
"""
import argparse
import hashlib
import io
from pathlib import Path
import tempfile
import urllib.request
import zipfile

VERSION = '1.1.46'
URL = 'https://github.com/KhronosGroup/OpenXR-SDK/releases/download/release-1.1.46/OpenXR.Loader.1.1.46.nupkg'
PACKAGE_SHA256 = 'bbbf8e0a63d7241c9186c7d52692c843151256fa9928a3e607fe1223fae0bce8'
DLL_SHA256 = 'a231a20944153cfda9551af135a3e58519f77007f28afc76d3c5d23763be8bde'
NOTICE_SHA256 = 'b57895e08b22074931f17605841dda431bd8e3c06ee7962f6a9371a5ff68431c'
MEMBER = 'native/x64/release/bin/openxr_loader.dll'
ROOT = Path(__file__).resolve().parents[1]
NOTICE = ROOT / 'third_party/openxr/LOADER-NOTICES.txt'
FILES = {'openxr_loader.dll': DLL_SHA256, 'OPENXR-LOADER-LICENSE.txt': NOTICE_SHA256}


def digest(data):
    return hashlib.sha256(data).hexdigest()


def pinned_dll(package, package_sha=PACKAGE_SHA256, dll_sha=DLL_SHA256):
    if digest(package) != package_sha:
        raise ValueError('OpenXR loader package hash differs from the pinned Khronos release')
    with zipfile.ZipFile(io.BytesIO(package)) as archive:
        data = archive.read(MEMBER)
    if digest(data) != dll_sha:
        raise ValueError('OpenXR loader DLL hash differs from the pinned x64 binary')
    return data


def verify(out):
    for name, expected in FILES.items():
        path = Path(out) / name
        if not path.is_file() or digest(path.read_bytes()) != expected:
            raise ValueError(str(path) + ' is missing or differs from the pinned dependency; run tools/fetch_openxr_loader.py')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, default=ROOT / 'third_party/openxr/loader')
    parser.add_argument('--archive', type=Path)
    parser.add_argument('--verify', action='store_true')
    parser.add_argument('--dry-run', action='store_true')
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    if args.verify:
        verify(args.out)
        print('OpenXR loader ' + VERSION + ': pinned DLL and notices verified')
        return 0
    notices = NOTICE.read_bytes()
    if digest(notices) != NOTICE_SHA256:
        raise ValueError('OpenXR loader notices do not match their provenance pin')
    if args.dry_run:
        print('Would fetch/verify ' + URL + ' and place the pinned loader and notices in ' + str(args.out))
        return 0
    if args.archive:
        package = args.archive.read_bytes()
    else:
        with urllib.request.urlopen(URL, timeout=60) as response:
            package = response.read(32 * 1024 * 1024 + 1)
        if len(package) > 32 * 1024 * 1024:
            raise ValueError('OpenXR loader package exceeds the download limit')
    dll = pinned_dll(package)
    args.out.mkdir(parents=True, exist_ok=True)
    for name, content in (('openxr_loader.dll', dll), ('OPENXR-LOADER-LICENSE.txt', notices)):
        with tempfile.NamedTemporaryFile(dir=args.out, delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(content)
        try:
            temporary.replace(args.out / name)
        finally:
            temporary.unlink(missing_ok=True)
    verify(args.out)
    print('Prepared official Khronos OpenXR loader ' + VERSION + ' in ' + str(args.out))
    return 0


def self_test():
    checks = 0
    with tempfile.TemporaryDirectory(prefix='edvr-openxr-loader-') as scratch:
        out = Path(scratch) / 'absent'
        assert main(['--out', str(out), '--dry-run']) == 0 and not out.exists()
        checks += 1
        try:
            verify(out)
            raise AssertionError('missing dependency accepted')
        except ValueError:
            checks += 1
        buffer = io.BytesIO()
        with zipfile.ZipFile(buffer, 'w') as archive:
            archive.writestr(MEMBER, b'test-loader')
            archive.writestr('../escape.dll', b'never extracted')
        package = buffer.getvalue()
        assert pinned_dll(package, digest(package), digest(b'test-loader')) == b'test-loader'
        assert not (Path(scratch) / 'escape.dll').exists()
        checks += 1
        for archive_hash, dll_hash in (('0' * 64, digest(b'test-loader')), (digest(package), '0' * 64)):
            try:
                pinned_dll(package, archive_hash, dll_hash)
                raise AssertionError('tampered dependency accepted')
            except ValueError:
                checks += 1
    assert digest(NOTICE.read_bytes()) == NOTICE_SHA256
    print('fetch_openxr_loader: %d checks passed (offline)' % (checks + 1))
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, ValueError, KeyError, zipfile.BadZipFile) as error:
        print('fetch_openxr_loader: ERROR: ' + str(error))
        raise SystemExit(1)

#!/usr/bin/env python3
"""Generate the installer's resource script -- payload, manifest, icon, version.

The installer ships as ONE executable with the files it installs embedded in
it, because the step people skip is "extract the zip first": Windows will
happily run a program from inside a zip, in a temporary folder, with none of
its siblings next to it. An installer that reads its payload from its own
directory works perfectly on every machine it is tested on and fails on the
first one where somebody double-clicked it in Explorer's zip view.

Generated rather than committed for two reasons:

  * The native graphics/runtime pair and bundled OpenXR loader are mandatory.
    A partial installer is worse than a build failure, so all native resources
    are validated before the generated directory is created.
  * nvngx_dlss.dll remains optional; it is included when the build provides it.
  * The version string comes from `git describe`, like the DLLs'.

Usage:
  python tools/gen_installer_rc.py --root <repo> --build <build dir>
                                   --out <gen dir> --version <string>
"""

import argparse
import os
import re
import struct
import subprocess
import sys

# Resource ids, matched by src/installer/payload.h.
IDR_NATIVE_GRAPHICS = 101
IDR_NATIVE_RUNTIME = 102
IDR_INI = 103
IDR_NGX = 104   # NVIDIA's DLSS runtime, when the build had the SDK
IDR_LOADER = 105
IDR_LOADER_NOTICE = 106
IDR_PROFILE = 107
RT_MANIFEST = 24


def parse_version_tuple(describe):
    """Parse a `git describe` string into a numeric (major, minor, patch,
    commits) tuple for FILEVERSION/PRODUCTVERSION.

    'v0.17.0-rc.2-16-g2c6e820' -> (0, 17, 0, 16): the trailing '-<n>-g<hash>'
    `git describe` appends past the tag is the commit count since it; a
    '-rc.N' pre-release suffix and a trailing '-dirty' are not fields
    VERSIONINFO has room for and are dropped either way. A bare tag
    ('v0.16.2', 'v0.17.0-rc.3') has no '-<n>-g<hash>' suffix and reads as
    zero commits since it. Anything that does not start with a dotted
    major.minor.patch -- 'unknown' (no repo) included -- is unparsable and
    becomes the all-zero version Windows shows for "no version". Every field
    is clamped to 0..65535, VERSIONINFO's numeric field width.
    """
    m = re.match(r'^v?(\d+)\.(\d+)\.(\d+)(?:-.*)?$', describe or '')
    if not m:
        return (0, 0, 0, 0)
    major, minor, patch = (int(m.group(i)) for i in (1, 2, 3))
    tail = re.search(r'-(\d+)-g[0-9a-f]+(?:-dirty)?$', describe)
    commits = int(tail.group(1)) if tail else 0

    def clamp(n):
        return max(0, min(65535, n))
    return tuple(clamp(n) for n in (major, minor, patch, commits))


def origin_url_https():
    """The origin remote as an https://github.com/... link with no trailing
    .git, for a Comments string that gives someone skeptical of an unsigned
    binary somewhere to check it. `git remote get-url origin` rather than a
    hand-typed constant for the same reason EDVR_VER is `git describe`: a
    repo that moves keeps a hand-typed URL that is quietly wrong. No git, no
    repo, or no `origin` remote falls back to the project's known home rather
    than failing a build over a Comments string.
    """
    fallback = 'https://github.com/characterecho-sean/edvr-unofficial-patch'
    try:
        url = subprocess.check_output(
            ['git', 'remote', 'get-url', 'origin'],
            stderr=subprocess.DEVNULL, timeout=10,
        ).decode('utf-8', 'replace').strip()
    except Exception:
        return fallback
    if url.endswith('.git'):
        url = url[:-len('.git')]
    m = re.match(r'^git@([^:]+):(.+)$', url)
    if m:
        return 'https://%s/%s' % (m.group(1), m.group(2))
    return url if url.startswith('http://') or url.startswith('https://') else fallback


def versioninfo_lines(version, file_description, internal_name, original_filename,
                      file_type, comments):
    """The VERSIONINFO block text, the single writer for it: every .rc file
    this module produces -- the installer's payload.rc and, now, the two
    DLLs' own standalone version_*.rc -- calls this rather than keeping its
    own copy of the eleven lines of StringFileInfo boilerplate.
    """
    major, minor, patch, commits = parse_version_tuple(version)
    return [
        '1 VERSIONINFO',
        'FILEVERSION %d,%d,%d,%d' % (major, minor, patch, commits),
        'PRODUCTVERSION %d,%d,%d,%d' % (major, minor, patch, commits),
        'FILEFLAGSMASK 0x3fL',
        'FILEFLAGS 0x0L',
        'FILEOS 0x40004L',
        'FILETYPE %s' % file_type,
        'FILESUBTYPE 0x0L',
        'BEGIN',
        '    BLOCK "StringFileInfo"',
        '    BEGIN',
        '        BLOCK "040904b0"',
        '        BEGIN',
        '            VALUE "CompanyName", "EDVR (unofficial)"',
        '            VALUE "FileDescription", "%s"' % file_description,
        '            VALUE "FileVersion", "%s"' % version,
        '            VALUE "InternalName", "%s"' % internal_name,
        '            VALUE "OriginalFilename", "%s"' % original_filename,
        '            VALUE "ProductName", "EDVR unofficial patch"',
        '            VALUE "ProductVersion", "%s"' % version,
        '            VALUE "LegalCopyright", "Copyright (c) 2026 characterecho-sean. MIT License."',
        '            VALUE "Comments", "%s"' % comments,
        '        END',
        '    END',
        '    BLOCK "VarFileInfo"',
        '    BEGIN',
        '        VALUE "Translation", 0x409, 1200',
        '    END',
        'END',
    ]


def write_version_rc(which, version, out, dry_run):
    """--version-rc graphics|runtime: a standalone .rc holding only a
    VERSIONINFO, for the two DLLs that otherwise ship with no resource naming
    them at all. No payload, no manifest, no icon, no --root/--build
    validation -- those exist to make sure the installer never carries a
    partial payload, and do not apply to a file that carries nothing but
    version strings.
    """
    comments = 'Source and releases: %s' % origin_url_https()
    if which == 'graphics':
        lines = versioninfo_lines(
            version,
            file_description=('EDVR unofficial VR patch for Elite Dangerous: Direct3D 11 '
                              'proxy (forwards to the system d3d11.dll)'),
            internal_name='edvr_openxr_graphics',
            original_filename='d3d11.dll',
            file_type='0x2L',
            comments=comments)
        filename = 'version_graphics.rc'
    else:
        lines = versioninfo_lines(
            version,
            file_description=('EDVR unofficial VR patch for Elite Dangerous: OpenXR runtime '
                              'speaking the OpenVR ABI'),
            internal_name='edvr_openxr_runtime',
            original_filename='openvr_api.dll',
            file_type='0x2L',
            comments=comments)
        filename = 'version_runtime.rc'

    if dry_run:
        print('gen_installer_rc: dry run (%s); wrote nothing' % which)
        return 0
    os.makedirs(out, exist_ok=True)
    out_path = os.path.join(out, filename)
    header = ['// Generated by tools/gen_installer_rc.py. Do not edit; do not commit.', '']
    with open(out_path, 'w', encoding='utf-8', newline='\r\n') as f:
        f.write('\n'.join(header + lines + ['']))
    print('gen_installer_rc: wrote %s' % out_path)
    return 0


def rc_path(path):
    """A path as an .rc string.

    Forward slashes: inside a quoted rc string a backslash is an escape, so
    "C:\\dir\\file.dll" would have to be doubled. rc.exe accepts forward
    slashes on Windows and this way there is nothing to double.
    """
    return os.path.abspath(path).replace('\\', '/')


def draw_icon(size):
    """One icon image, as BGRA rows bottom-up: two overlapping stereo circles.

    Drawn rather than committed so the repository stays free of binary assets
    for something this small. The mark is the same idea as the fixes: two eyes
    that should agree, overlapping.
    """
    px = [[(0, 0, 0, 0)] * size for _ in range(size)]

    def blend(dst, src):
        sa = src[3] / 255.0
        if sa <= 0:
            return dst
        out = []
        for i in range(3):
            out.append(int(round(src[i] * sa + dst[i] * (1 - sa))))
        out.append(max(dst[3], src[3]))
        return tuple(out)

    r = size * 0.5
    # Rounded background square.
    radius = size * 0.22
    for y in range(size):
        for x in range(size):
            cx = min(max(x + 0.5, radius), size - radius)
            cy = min(max(y + 0.5, radius), size - radius)
            dx, dy = x + 0.5 - cx, y + 0.5 - cy
            if dx * dx + dy * dy <= radius * radius + 0.5:
                px[y][x] = (28, 22, 18, 255)  # BGRA: near-black navy

    def circle(ox, oy, rad, colour):
        for y in range(size):
            for x in range(size):
                dx, dy = x + 0.5 - ox, y + 0.5 - oy
                d = (dx * dx + dy * dy) ** 0.5
                if d <= rad:
                    # A soft edge so small sizes do not look ragged.
                    a = 255 if d <= rad - 1 else int(255 * (rad - d))
                    if a > 0:
                        px[y][x] = blend(px[y][x], (colour[0], colour[1], colour[2], a))

    eye = size * 0.235
    circle(r - size * 0.145, r, eye, (255, 175, 87))    # BGR: cyan-blue #57afff
    circle(r + size * 0.145, r, eye, (67, 159, 255))    # BGR: amber #ff9f43
    return px


def ico_bytes():
    """A .ico holding 16, 32 and 48 pixel images."""
    images = []
    for size in (16, 32, 48):
        px = draw_icon(size)
        # BITMAPINFOHEADER with doubled height, then BGRA bottom-up, then the
        # AND mask (all zero: the alpha channel carries transparency).
        header = struct.pack('<IiiHHIIiiII', 40, size, size * 2, 1, 32, 0, size * size * 4,
                             0, 0, 0, 0)
        body = bytearray()
        for y in range(size - 1, -1, -1):
            for x in range(size):
                b, g, r, a = px[y][x]
                body += struct.pack('<BBBB', b, g, r, a)
        mask_row = ((size + 31) // 32) * 4
        body += bytes(mask_row * size)
        images.append((size, header + bytes(body)))

    out = bytearray(struct.pack('<HHH', 0, 1, len(images)))
    offset = 6 + 16 * len(images)
    for size, data in images:
        out += struct.pack('<BBBBHHII', size % 256, size % 256, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    for _size, data in images:
        out += data
    return bytes(out)


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--root')
    ap.add_argument('--build')
    ap.add_argument('--out')
    ap.add_argument('--version', default='unknown')
    ap.add_argument('--profile', choices=('vr', 'flat'), default='vr')
    ap.add_argument('--ini', help='profile-specific shipped INI (default: root edvr.ini)')
    ap.add_argument('--version-rc', choices=('graphics', 'runtime'),
                    help='write a standalone VERSIONINFO .rc for this DLL instead of '
                         'the installer payload')
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('--self-test', action='store_true')
    args = ap.parse_args(argv)
    if args.self_test:
        return self_test()
    if args.version_rc:
        if not args.out:
            ap.error('--out is required with --version-rc')
        return write_version_rc(args.version_rc, args.version, args.out, args.dry_run)
    if not args.root or not args.build or not args.out:
        ap.error('--root, --build, and --out are required')

    d3d11 = os.path.join(args.build, 'edvr_openxr_graphics.dll')
    runtime = os.path.join(args.build, 'edvr_openxr_runtime.dll')
    loader = os.path.join(args.build, 'openxr_loader.dll')
    notice = os.path.join(args.build, 'OPENXR-LOADER-LICENSE.txt')
    ini = args.ini or (os.path.join(args.build, 'edvr-flat.ini') if args.profile == 'flat'
                       else os.path.join(args.root, 'edvr.ini'))
    ngx = os.path.join(args.build, 'nvngx_dlss.dll')
    manifest = os.path.join(args.root, 'src', 'installer', 'installer.manifest')

    required = [("native graphics", d3d11)]
    if args.profile != 'flat' or args.ini:
        required.append(("INI", ini))
    if args.profile == 'vr':
        required += [("native runtime", runtime), ("OpenXR loader", loader),
                     ("OpenXR loader notice", notice)]
    missing = [(label, path) for label, path in required if not os.path.isfile(path)]
    if missing:
        for label, path in missing:
            print('gen_installer_rc: ERROR: %s (%s) is not there; build it first' % (label, path))
        return 1
    try:
        import openxr_pe
        openxr_pe.native_graphics_exports(d3d11)
        if args.profile == 'vr':
            openxr_pe.native_exports(runtime)
            from fetch_openxr_loader import verify
            verify(args.build)
    except (ImportError, OSError, ValueError) as exc:
        print('gen_installer_rc: ERROR: native payload validation failed: %s' % exc)
        return 1

    # The manifest has to be valid XML or Windows refuses to start the program
    # at all -- "the side-by-side configuration is incorrect", before a line of
    # its code runs. It links and packages perfectly happily, so nothing else in
    # the build would notice. It has already happened once, to a comment
    # containing the double hyphen this project uses as an em dash, which XML
    # does not allow inside comments.
    try:
        import xml.etree.ElementTree as ElementTree
        ElementTree.parse(manifest)
    except Exception as exc:  # noqa: BLE001 -- any parse failure is fatal here
        print('gen_installer_rc: ERROR: %s is not valid XML: %s' % (manifest, exc))
        print('  A manifest Windows cannot parse is an installer that cannot start.')
        return 1

    if args.dry_run:
        print('gen_installer_rc: dry run; validated %s resources and wrote nothing' % args.profile)
        return 0
    os.makedirs(args.out, exist_ok=True)
    if args.profile == 'flat' and not args.ini:
        with open(ini, 'wb') as f:
            f.write(b'# Experimental flat temporal profile: visual qualification in progress.\r\n'
                    b'# F8 opens the AA menu: Off / TAA / DLSS / FSR3 and DLSS model presets.\r\n'
                    b'# temporal_aa: off, on (TAA), dlaa (native SS), dlss, fsr. Game SS controls render scale.\r\n'
                    b'# Press F10 in the cockpit to collect one bounded flat scene capture.\r\n'
                    b'[fix]\r\ntemporal_aa = off\r\ntemporal_aa_model = k\r\n\r\n'
                    b'[hotkey]\r\nmenu = F8\r\ndump_draws = F10\r\n\r\n'
                    b'[log]\r\nenabled = 1\r\n\r\n'
                    b'[advanced]\r\nreal_dll =\r\n')
    if args.profile == 'flat':
        with open(os.path.join(args.build, 'edvr-flat-README.txt'), 'wb') as f:
            f.write(b'EDVR flat temporal AA qualification build\r\n\r\n'
                    b'Run edvr-flat-installer.exe. This edition installs d3d11.dll, edvr.ini,\r\n'
                    b'and edvr_profile.ini beside EliteDangerous64.exe. No VR runtime is installed.\r\n'
                    b'Experimental temporal AA; visual quality is not yet qualified.\r\n'
                    b'Press F8 for the AA menu. Up/Down selects a row; Left/Right changes it.\r\n'
                    b'Choose Off, TAA, DLSS or FSR3 and the DLSS model preset; settings save live.\r\n'
                    b'Press F8 or Escape to close. Game keys are private while the menu is drawn.\r\n'
                    b'[fix] temporal_aa also accepts off, on (TAA), dlaa, dlss or fsr (default off).\r\n'
                    b'Game supersampling controls render scale; DLAA requires native SS.\r\n'
                    b'Press F10 in the cockpit to collect one bounded flat scene capture.\r\n'
                    b'Use --convert-profile for an explicit VR/flat edition switch.\r\n')
    icon_path = os.path.join(args.out, 'edvr_installer.ico')
    with open(icon_path, 'wb') as f:
        f.write(ico_bytes())
    descriptor_path = os.path.join(args.build, 'edvr_profile_%s.ini' % args.profile)
    with open(descriptor_path, 'wb') as f:
        f.write(('[install]\r\nschema = 1\r\nprofile = %s\r\n' % args.profile).encode('ascii'))

    lines = [
        '// Generated by tools/gen_installer_rc.py. Do not edit; do not commit.',
        '',
        '1 %d "%s"' % (RT_MANIFEST, rc_path(manifest)),
        '1 ICON "%s"' % rc_path(icon_path),
        '',
        '%d RCDATA "%s"' % (IDR_NATIVE_GRAPHICS, rc_path(d3d11)),
        '%d RCDATA "%s"' % (IDR_INI, rc_path(ini)),
        '%d RCDATA "%s"' % (IDR_PROFILE, rc_path(descriptor_path)),
    ]
    carried = 'graphics, edvr.ini and %s profile descriptor' % args.profile
    if args.profile == 'vr':
        lines += ['%d RCDATA "%s"' % (IDR_NATIVE_RUNTIME, rc_path(runtime)),
                  '%d RCDATA "%s"' % (IDR_LOADER, rc_path(loader)),
                  '%d RCDATA "%s"' % (IDR_LOADER_NOTICE, rc_path(notice))]
        carried += ', native OpenXR runtime, loader and notice'
    if os.path.exists(ngx):
        lines.append('%d RCDATA "%s"' % (IDR_NGX, rc_path(ngx)))
        carried += ", and NVIDIA's DLSS runtime (nvngx_dlss.dll)"
    else:
        lines.append('// no nvngx_dlss.dll in the build (no DLSS SDK): this installer ships without')
        lines.append("// NVIDIA's optional anti-aliasing runtime.")

    version = args.version
    lines += [''] + versioninfo_lines(
        version,
        file_description='EDVR flat temporal qualification installer' if args.profile == 'flat' else 'EDVR installer',
        internal_name='edvr-flat-installer' if args.profile == 'flat' else 'edvr-installer',
        original_filename='edvr-flat-installer.exe' if args.profile == 'flat' else 'edvr-installer.exe',
        file_type='0x1L',
        comments='Carries %s' % carried) + ['']

    out_path = os.path.join(args.out, 'payload.rc')
    with open(out_path, 'w', encoding='utf-8', newline='\r\n') as f:
        f.write('\n'.join(lines))
    print('gen_installer_rc: wrote %s (%s)' % (out_path, carried))
    return 0


def self_test():
    import tempfile

    # The describe-string -> numeric-tuple parser, used for FILEVERSION and
    # PRODUCTVERSION in every VERSIONINFO block this module writes.
    assert parse_version_tuple('v0.17.0-rc.2-16-g2c6e820') == (0, 17, 0, 16)
    assert parse_version_tuple('v0.16.2') == (0, 16, 2, 0)
    assert parse_version_tuple('v0.17.0-rc.3') == (0, 17, 0, 0)
    assert parse_version_tuple('unknown') == (0, 0, 0, 0)
    assert parse_version_tuple('garbage') == (0, 0, 0, 0)
    assert parse_version_tuple('v1.2.3-70000-gabcdef1') == (1, 2, 3, 65535)  # clamped

    with tempfile.TemporaryDirectory(prefix='edvr-rc-test-') as scratch:
        root = os.path.join(scratch, 'root'); build = os.path.join(root, 'build')
        out = os.path.join(root, 'generated'); os.makedirs(build)
        os.makedirs(os.path.join(root, 'src', 'installer'))
        for name in ('edvr_openxr_graphics.dll', 'edvr_openxr_runtime.dll', 'openxr_loader.dll',
                     'OPENXR-LOADER-LICENSE.txt'):
            with open(os.path.join(build, name), 'wb') as stream: stream.write(b'x')
        with open(os.path.join(root, 'edvr.ini'), 'wb') as stream: stream.write(b'[openxr]\n')
        with open(os.path.join(root, 'src', 'installer', 'installer.manifest'), 'w') as stream:
            stream.write('<assembly></assembly>')
        import openxr_pe, fetch_openxr_loader
        old_g, old_r, old_v = openxr_pe.native_graphics_exports, openxr_pe.native_exports, fetch_openxr_loader.verify
        openxr_pe.native_graphics_exports = lambda path: None; openxr_pe.native_exports = lambda path: None
        fetch_openxr_loader.verify = lambda path: None
        try:
            assert main(['--root', root, '--build', build, '--out', out,
                         '--version', '1.2.3', '--dry-run']) == 0 and not os.path.exists(out)
            os.remove(os.path.join(build, 'edvr_openxr_runtime.dll'))
            assert main(['--root', root, '--build', build, '--out', out,
                         '--version', '1.2.3']) == 1 and not os.path.exists(out)
            assert main(['--root', root, '--build', build, '--out', out,
                         '--version', '1.2.3', '--profile', 'flat', '--dry-run']) == 0
            assert not os.path.exists(out) and not os.path.exists(os.path.join(build, 'edvr-flat.ini'))
            assert main(['--root', root, '--build', build, '--out', out,
                         '--version', '1.2.3', '--profile', 'flat']) == 0
            with open(os.path.join(out, 'payload.rc'), encoding='utf-8') as stream:
                rc_text = stream.read()
            assert '%d RCDATA ' % IDR_NATIVE_RUNTIME not in rc_text
            assert '%d RCDATA ' % IDR_LOADER not in rc_text
            assert '%d RCDATA ' % IDR_LOADER_NOTICE not in rc_text
            assert '%d RCDATA ' % IDR_PROFILE in rc_text
            with open(os.path.join(build, 'edvr-flat.ini'), 'rb') as stream:
                flat_ini = stream.read()
                assert b'temporal_aa = off' in flat_ini
                assert b'temporal_aa_model = k' in flat_ini and b'menu = F8' in flat_ini
            with open(os.path.join(build, 'edvr_profile_flat.ini'), 'rb') as stream:
                assert stream.read() == b'[install]\r\nschema = 1\r\nprofile = flat\r\n'
        finally:
            openxr_pe.native_graphics_exports, openxr_pe.native_exports = old_g, old_r
            fetch_openxr_loader.verify = old_v

        # --version-rc: a standalone VERSIONINFO .rc, no native payload involved.
        rc_out = os.path.join(root, 'rc_out')
        assert main(['--version-rc', 'graphics', '--version', 'v0.17.0-rc.2-16-g2c6e820',
                     '--out', rc_out, '--dry-run']) == 0 and not os.path.exists(rc_out)
        assert main(['--version-rc', 'graphics', '--version', 'v0.17.0-rc.2-16-g2c6e820',
                     '--out', rc_out]) == 0
        with open(os.path.join(rc_out, 'version_graphics.rc'), 'r', encoding='utf-8') as stream:
            graphics_text = stream.read()
        assert 'OriginalFilename", "d3d11.dll"' in graphics_text
        assert 'FILEVERSION 0,17,0,16' in graphics_text

        assert main(['--version-rc', 'runtime', '--version', 'v0.16.2', '--out', rc_out]) == 0
        with open(os.path.join(rc_out, 'version_runtime.rc'), 'r', encoding='utf-8') as stream:
            runtime_text = stream.read()
        assert 'OriginalFilename", "openvr_api.dll"' in runtime_text
        assert 'FILEVERSION 0,16,2,0' in runtime_text
    print('gen_installer_rc: self-test passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())

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
import struct
import sys

# Resource ids, matched by src/installer/payload.h.
IDR_NATIVE_GRAPHICS = 101
IDR_NATIVE_RUNTIME = 102
IDR_INI = 103
IDR_NGX = 104   # NVIDIA's DLSS runtime, when the build had the SDK
IDR_LOADER = 105
IDR_LOADER_NOTICE = 106
RT_MANIFEST = 24


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
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('--self-test', action='store_true')
    args = ap.parse_args(argv)
    if args.self_test:
        return self_test()
    if not args.root or not args.build or not args.out:
        ap.error('--root, --build, and --out are required')

    d3d11 = os.path.join(args.build, 'edvr_openxr_graphics.dll')
    runtime = os.path.join(args.build, 'edvr_openxr_runtime.dll')
    loader = os.path.join(args.build, 'openxr_loader.dll')
    notice = os.path.join(args.build, 'OPENXR-LOADER-LICENSE.txt')
    ini = os.path.join(args.root, 'edvr.ini')
    ngx = os.path.join(args.build, 'nvngx_dlss.dll')
    manifest = os.path.join(args.root, 'src', 'installer', 'installer.manifest')

    required = (("native graphics", d3d11), ("native runtime", runtime),
                ("OpenXR loader", loader), ("OpenXR loader notice", notice),
                ("INI", ini))
    missing = [(label, path) for label, path in required if not os.path.isfile(path)]
    if missing:
        for label, path in missing:
            print('gen_installer_rc: ERROR: %s (%s) is not there; build it first' % (label, path))
        return 1
    try:
        import openxr_pe
        openxr_pe.native_graphics_exports(d3d11)
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
        print('gen_installer_rc: dry run; validated native resources and wrote nothing')
        return 0
    os.makedirs(args.out, exist_ok=True)
    icon_path = os.path.join(args.out, 'edvr_installer.ico')
    with open(icon_path, 'wb') as f:
        f.write(ico_bytes())

    lines = [
        '// Generated by tools/gen_installer_rc.py. Do not edit; do not commit.',
        '',
        '1 %d "%s"' % (RT_MANIFEST, rc_path(manifest)),
        '1 ICON "%s"' % rc_path(icon_path),
        '',
        '%d RCDATA "%s"' % (IDR_NATIVE_GRAPHICS, rc_path(d3d11)),
        '%d RCDATA "%s"' % (IDR_NATIVE_RUNTIME, rc_path(runtime)),
        '%d RCDATA "%s"' % (IDR_INI, rc_path(ini)),
        '%d RCDATA "%s"' % (IDR_LOADER, rc_path(loader)),
        '%d RCDATA "%s"' % (IDR_LOADER_NOTICE, rc_path(notice)),
    ]
    carried = 'native graphics/runtime pair, OpenXR loader, loader notice and edvr.ini'
    if os.path.exists(ngx):
        lines.append('%d RCDATA "%s"' % (IDR_NGX, rc_path(ngx)))
        carried += ", and NVIDIA's DLSS runtime (nvngx_dlss.dll)"
    else:
        lines.append('// no nvngx_dlss.dll in the build (no DLSS SDK): this installer ships without')
        lines.append("// NVIDIA's optional anti-aliasing runtime; native OpenXR remains complete.")

    version = args.version
    lines += [
        '',
        '1 VERSIONINFO',
        'FILEVERSION 0,0,0,0',
        'PRODUCTVERSION 0,0,0,0',
        'FILEFLAGSMASK 0x3fL',
        'FILEFLAGS 0x0L',
        'FILEOS 0x40004L',
        'FILETYPE 0x1L',
        'FILESUBTYPE 0x0L',
        'BEGIN',
        '    BLOCK "StringFileInfo"',
        '    BEGIN',
        '        BLOCK "040904b0"',
        '        BEGIN',
        '            VALUE "CompanyName", "EDVR (unofficial)"',
        '            VALUE "FileDescription", "EDVR installer"',
        '            VALUE "FileVersion", "%s"' % version,
        '            VALUE "InternalName", "edvr-installer"',
        '            VALUE "OriginalFilename", "edvr-installer.exe"',
        '            VALUE "ProductName", "EDVR unofficial patch"',
        '            VALUE "ProductVersion", "%s"' % version,
        '            VALUE "Comments", "Carries %s"' % carried,
        '        END',
        '    END',
        '    BLOCK "VarFileInfo"',
        '    BEGIN',
        '        VALUE "Translation", 0x409, 1200',
        '    END',
        'END',
        '',
    ]

    out_path = os.path.join(args.out, 'payload.rc')
    with open(out_path, 'w', encoding='utf-8', newline='\r\n') as f:
        f.write('\n'.join(lines))
    print('gen_installer_rc: wrote %s (%s)' % (out_path, carried))
    return 0


def self_test():
    import tempfile
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
        finally:
            openxr_pe.native_graphics_exports, openxr_pe.native_exports = old_g, old_r
            fetch_openxr_loader.verify = old_v
    print('gen_installer_rc: self-test passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())

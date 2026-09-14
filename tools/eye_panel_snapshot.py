#!/usr/bin/env python3
"""Read bounded EDVRPNL1 evidence; distinguish retained data from replayable geometry."""
from __future__ import annotations
import argparse
import dataclasses
import json
import struct
from pathlib import Path

MAGIC = b"EDVRPNL1"
CAP = 768 * 1024 * 1024
VS, PS = 0x81216C77F90DEDD6, 0xA2965EC2931A39C8


class PanelSnapshotError(ValueError):
    pass


@dataclasses.dataclass
class Blob:
    name: str
    meta: dict
    payload: memoryview


@dataclasses.dataclass
class Draw:
    info: dict
    blobs: dict[str, Blob]


@dataclasses.dataclass
class Snapshot:
    header: dict
    draws: list[Draw]
    final_failures: int


def require(condition, message):
    if not condition:
        raise PanelSnapshotError(message)


def number(value, name, maximum=0xffffffff):
    require(type(value) is int and 0 <= value <= maximum, f"invalid {name}: {value!r}")
    return value


class Reader:
    def __init__(self, data):
        self.data, self.pos = memoryview(data), 0

    def take(self, n):
        require(0 <= n <= len(self.data)-self.pos, f"truncated data at {self.pos}, need {n}")
        out = self.data[self.pos:self.pos+n]
        self.pos += n
        return out

    def integer(self, fmt="I"):
        return struct.unpack("<"+fmt, self.take(struct.calcsize(fmt)))[0]

    def text(self, limit=4*1024*1024):
        n = self.integer()
        require(n <= limit, "oversized string")
        return bytes(self.take(n)).decode("utf-8")

    def obj(self):
        value = json.loads(self.text())
        require(isinstance(value, dict), "metadata must be a JSON object")
        return value


def read_bytes(data):
    r = Reader(data)
    require(bytes(r.take(8)) == MAGIC, "bad panel magic")
    names = ("version", "draw_count", "first_frame", "declined", "capture_failures",
             "ignored_other_eye", "ignored_other_frame")
    header = {name:r.integer() for name in names}
    require(header["version"] == 1 and header["draw_count"] <= 16, "unsupported version/count")
    header.update(target_identity=r.integer("Q"), reserved_bytes=r.integer("Q"))
    require(header["reserved_bytes"] <= CAP, "reserved payload exceeds cap")
    draws, retained = [], 0
    for _ in range(header["draw_count"]):
        info, blobs = r.obj(), {}
        count = r.integer()
        require(count <= 64, "too many panel blobs")
        for _ in range(count):
            name, meta, size = r.text(128), r.obj(), r.integer()
            require(name and name not in blobs, "empty/duplicate blob name")
            retained += size
            require(retained <= header["reserved_bytes"], "payload exceeds reservation")
            blobs[name] = Blob(name, meta, r.take(size))
        draws.append(Draw(info, blobs))
    failures = r.integer()
    require(failures >= header["capture_failures"], "failure counter went backwards")
    require(r.pos == len(r.data), "trailing data")
    return Snapshot(header, draws, failures)


def read_snapshot(path):
    return read_bytes(Path(path).read_bytes())


def descriptor(value, words, name, optional=False):
    if value is None and optional:
        return
    require(isinstance(value, list) and len(value) == words, f"invalid {name} descriptor")
    for item in value:
        number(item, name)


# Captured texture storage formats, including packed D32F/S8 depth.
PIXEL_BYTES = {2:16, 10:8, 11:8, 19:8, 20:8, 26:4, 28:4, 29:4, 41:4, 61:1}
BLOCK_BYTES = {71:8, 72:8}


def verify_blob(blob):
    m, payload, name = blob.meta, blob.payload, blob.name
    require(type(m.get("complete")) is bool, f"{name}: missing complete flag")
    expected = number(m.get("bytes"), name+".bytes", CAP)
    reason = m.get("reason")
    require(isinstance(reason, str), f"{name}: missing reason")
    require(m.get("type") in ("texture", "buffer", "shader"), f"{name}: unknown type")
    if not m["complete"]:
        require(not payload and reason, f"{name}: unavailable payload must be empty with a reason")
        return
    require(expected > 0 and len(payload) == expected and not reason, f"{name}: payload/flag mismatch")
    if m["type"] == "shader":
        require(m.get("hash") in (VS, PS), f"{name}: shader identity")
        require(bytes(payload[:4]) == b"DXBC", f"{name}: not DXBC")
        return
    number(m.get("resource"), name+".resource", 0xffffffffffffffff)
    desc = m.get("resource_desc")
    descriptor(desc, 11 if m["type"] == "texture" else 6, name+".resource")
    if m["type"] == "buffer":
        off, whole = number(m.get("offset"), name+".offset"), number(m.get("whole"), name+".whole")
        require(whole == desc[0] and off+expected <= whole, f"{name}: buffer window outside resource")
        if "minimum" in m:
            require(expected >= number(m["minimum"], name+".minimum"), f"{name}: short CB")
        if name in ("vs_t33", "vs_t38"):
            stride, view = (336 if name == "vs_t33" else 48), m.get("view")
            descriptor(view, 6, name+".srv")
            require(desc[5] == stride and m.get("stride") == stride, f"{name}: structured stride")
            require(view[0:2] == [0, 1] and view[3] > 0 and (view[2]+view[3])*stride <= whole,
                    f"{name}: structured SRV range")
            require(off == 0 and expected == whole, f"{name}: structured data is incomplete")
        return
    fmt = number(m.get("storage_format"), name+".format")
    require(fmt in PIXEL_BYTES or fmt in BLOCK_BYTES, f"{name}: unsupported texture format {fmt}")
    origin, mips = m.get("origin"), m.get("mips")
    require(isinstance(origin, list) and len(origin) == 2, f"{name}: crop origin")
    ox, oy = (number(v, name+".origin") for v in origin)
    require(isinstance(mips, list) and mips, f"{name}: no mip metadata")
    require(desc[3] == 1 and desc[5] == 1, f"{name}: unsupported array/MSAA")
    cropped = name in ("rtv_before", "rtv_after", "dsv_roi")
    require(len(mips) == (1 if cropped else desc[2]), f"{name}: missing mip levels")
    total = 0
    for level, mip in enumerate(mips):
        vals = {k:number(mip.get(k), name+"."+k) for k in
                ("level", "width", "height", "row_bytes", "rows", "offset", "size")}
        w, h = max(1, desc[0] >> level), max(1, desc[1] >> level)
        if cropped:
            w, h = min(1400, w), min(1400, h)
            require((ox, oy) == ((desc[0]-w)//2, (desc[1]-h)//2), f"{name}: wrong crop origin")
        else:
            require((ox, oy) == (0, 0), f"{name}: sampled texture was cropped")
        row = ((w+3)//4)*BLOCK_BYTES[fmt] if fmt in BLOCK_BYTES else w*PIXEL_BYTES[fmt]
        rows = (h+3)//4 if fmt in BLOCK_BYTES else h
        require((vals["level"], vals["width"], vals["height"], vals["row_bytes"], vals["rows"], vals["offset"], vals["size"])
                == (level, w, h, row, rows, total, row*rows), f"{name}: mip {level} extent/packing")
        total += row*rows
    require(expected == total, f"{name}: mip payload size")
    if not cropped:
        view = m.get("view")
        descriptor(view, 6, name+".srv")
        require(view[1] == 4, f"{name}: not Texture2D SRV")
        first, levels = view[2], view[3]
        if levels == 0xffffffff:
            levels = desc[2]-first
        require(0 <= first < desc[2] and 0 < levels <= desc[2]-first, f"{name}: invalid mip view")
        require((m.get("view_first_mip"), m.get("view_mips")) == (first, levels), f"{name}: normalized mip view")


# Size and natural alignment of the IA formats used by this family and fixture.
IA = {2:(16,4),3:(16,4),4:(16,4),6:(12,4),7:(12,4),8:(12,4),
      10:(8,2),11:(8,2),12:(8,2),13:(8,2),14:(8,2),16:(8,4),17:(8,4),18:(8,4),
      24:(4,4),25:(4,4),26:(4,4),28:(4,1),29:(4,1),30:(4,1),31:(4,1),32:(4,1),
      34:(4,2),35:(4,2),36:(4,2),37:(4,2),38:(4,2),41:(4,4),42:(4,4),43:(4,4),
      49:(2,1),50:(2,1),51:(2,1),52:(2,1),54:(2,2),56:(2,2),57:(2,2),58:(2,2),59:(2,2),
      61:(1,1),62:(1,1),63:(1,1),64:(1,1)}


def verify_geometry(draw):
    d, blobs = draw.info, draw.blobs
    count, instances, start, si = (number(d.get(k), "draw."+k) for k in ("count", "instances", "start", "start_instance"))
    base = d.get("base")
    require(type(base) is int and -0x80000000 <= base < 0x80000000, "invalid signed base")
    require(count > 0 and instances > 0, "empty draw")
    kind = d.get("kind")
    require(kind in map(ord, "XINV"), "unsupported draw kind")
    if kind in (ord("X"), ord("I")):
        ib = blobs.get("ib")
        require(ib is not None and ib.meta["complete"], "IB unavailable")
        fmt = ib.meta.get("format")
        require(fmt in (42,57), "unsupported index format")
        width, code = (2,"H") if fmt == 57 else (4,"I")
        require(ib.meta.get("offset") == ib.meta.get("binding_offset",0)+start*width, "IB capture origin")
        require(len(ib.payload) == count*width, "IB exact draw window")
        indices = [item[0] for item in struct.iter_unpack("<"+code, ib.payload)]
        if d.get("topology") in (3,5,11,13):
            indices = [i for i in indices if i != (1 << (width*8))-1]
        require(indices, "draw contains only strip cuts")
        lo, hi = min(indices)+base, max(indices)+base
        require(lo >= 0, "negative vertex index after base")
    else:
        lo, hi = start, start+count-1
    layout = d.get("layout")
    require(isinstance(layout, list) and 0 < len(layout) <= 32, "layout unavailable")
    cursor, semantics = {}, set()
    for item in layout:
        require(isinstance(item,list) and len(item)==7, "malformed input element")
        sem, idx, fmt, slot, off, cls, step = item
        require(isinstance(sem,str) and sem, "invalid semantic")
        for k,v in (("index",idx),("slot",slot),("offset",off),("class",cls),("step",step)):
            number(v,"layout."+k)
        require((sem.upper(),idx) not in semantics, "duplicate semantic")
        semantics.add((sem.upper(),idx))
        require(slot < 32 and fmt in IA and cls in (0,1), "unsupported input element")
        size, alignment = IA[fmt]
        if off == 0xffffffff:
            off = (cursor.get(slot,0)+alignment-1)//alignment*alignment
        cursor[slot] = off+size
        vb = blobs.get(f"vb{slot}")
        require(vb is not None and vb.meta["complete"], f"vb{slot} unavailable")
        m = vb.meta
        stride, binding, captured = (number(m.get(k),f"vb{slot}."+k) for k in ("stride","binding_offset","offset"))
        require(stride > 0 and (m.get("inputclass"),m.get("step")) == (cls,step), "VB layout/binding mismatch")
        if cls:
            require(step > 0, "instance step zero not captured")
            first, last = si, si+(instances-1)//step
        else:
            require(step == 0, "per-vertex step must be zero")
            first, last = lo, hi
        low, high = binding+first*stride+off, binding+last*stride+off+size
        require(captured <= low and high <= captured+len(vb.payload), f"vb{slot}: referenced element outside retained window")
    return True


def verify_snapshot(snapshot):
    reports = []
    for i, draw in enumerate(snapshot.draws):
        d = draw.info
        require(type(d.get("complete")) is bool and isinstance(d.get("reasons"), list), "draw completeness metadata")
        for blob in draw.blobs.values():
            verify_blob(blob)
        required = {"rtv_before","rtv_after","ps_t0","ps_t1","ps_t2","vs_t33","vs_t38",
                    "vs_b0","vs_b1","vs_b2","ps_b1","ps_b2","shader_vs","shader_ps"}
        if d.get("dsv_view") is not None:
            required.add("dsv_roi")
        missing = sorted(required-draw.blobs.keys())
        if d["complete"]:
            require(not missing and not d["reasons"] and all(b.meta["complete"] for b in draw.blobs.values()), "false complete draw")
            require((d.get("vs"),d.get("ps")) == (VS,PS), "unexpected shader pair")
            for key,n,opt in (("rtv_view",5,False),("dsv_view",6,True),("blend",66,True),
                              ("depth",13,True),("raster",10,True),("blend_factor",4,False)):
                descriptor(d.get(key),n,key,opt)
            require(isinstance(d.get("viewports"),list) and len(d["viewports"]) <= 16, "viewports")
            for vp in d["viewports"]: descriptor(vp,6,"viewport")
            require(isinstance(d.get("scissors"),list) and len(d["scissors"]) <= 16, "scissors")
            for sc in d["scissors"]: descriptor(sc,4,"scissor")
            require(isinstance(d.get("samplers"),list) and len(d["samplers"]) == 2, "samplers")
            for sampler in d["samplers"]: descriptor(sampler,13,"sampler",True)
        geometry, why = False, ""
        try:
            geometry = verify_geometry(draw)
        except PanelSnapshotError as e:
            why = str(e)
        reports.append({"draw":i,"frame":d.get("frame"),"ordinal":d.get("ordinal"),
                        "inputs_complete":d["complete"],"geometry_verified":geometry,
                        "geometry_reason":why,"missing":missing,
                        "replay_inputs_complete":bool(d["complete"] and geometry)})
    return {"header":snapshot.header,"final_failures":snapshot.final_failures,"draws":reports}


def verify_fixture(path):
    snap = read_snapshot(path)
    report = verify_snapshot(snap)
    require(snap.draws and all(d["replay_inputs_complete"] for d in report["draws"]), "fixture not fully retained/replayable")
    require(snap.final_failures == 0, "fixture readback failure")
    first = snap.draws[0]
    require(first.blobs["rtv_before"].payload != first.blobs["rtv_after"].payload, "native draw did not change colour")
    require(first.info["start"] and first.info["base"] and first.info["start_instance"] and first.info["instances"] > 1,
            "fixture omits draw offset/instancing cases")
    require(first.blobs["ps_t1"].meta["mips"][-1]["width"] == 1, "fixture lacks BC1 sub-block mips")
    require(snap.header["first_frame"] == 9001 and snap.header["ignored_other_eye"] == 1 and
            snap.header["ignored_other_frame"] == 1, "fixture frame/eye accounting")
    def pattern(size, seed):
        return bytes((seed+i*13) & 255 for i in range(size))
    for name, size, seed in (("vs_b0",192,1),("vs_b1",5376,2),("vs_b2",64,3),
                             ("ps_b1",5232,4),("ps_b2",208,5),("vs_t33",1344,6),("vs_t38",384,7)):
        require(first.blobs[name].payload == pattern(size,seed), f"{name}: lost draw-local buffer contents")
    texture_patterns = {
        "ps_t0":((2048,0x11),(512,0x22),(128,0x33),(32,0x44),(8,0x55)),
        "ps_t1":((32,0x61),(8,0x62),(8,0x63),(8,0x64)),
        "ps_t2":((196,0x71),(36,0x72),(4,0x73))}
    for name, levels in texture_patterns.items():
        require(first.blobs[name].payload == b"".join(pattern(n,seed) for n,seed in levels),
                f"{name}: lost original mip data after source overwrite")
    require([first.blobs[name].meta["resource_desc"][4] for name in texture_patterns] == [9,70,27],
            "fixture does not exercise native typeless storage")
    require(first.blobs["ps_t0"].meta["view_first_mip"] == 1, "fixture lacks a nonzero mip view")
    for name in ("vs_t33","vs_t38"):
        require(first.blobs[name].meta["view"][2] == 1, "fixture lacks structured subset views")
    require(first.blobs["ib"].payload == struct.pack("<4H",0,1,2,3), "index window was not retained")
    require(first.blobs["vb0"].payload[:24] == struct.pack("<6f",-.6,-.6,0,0,1,0), "vertex origin/data changed")
    require(first.blobs["vb1"].payload[:32] == struct.pack("<8f",-.1,0,0,0,.1,0,0,0), "instance origin/data changed")
    require(not any(first.blobs["rtv_before"].payload), "before crop not from original target")
    after = first.blobs["rtv_after"].payload
    require(not any(after[:4]) and any(after), "after crop must retain draw plus untouched border, not later clear")
    depth = first.blobs["dsv_roi"].payload
    require(all(z == 0x3e800000 and stencil & 255 == 5 for z,stencil in struct.iter_unpack("<II",depth)),
            "depth/stencil crop changed after source clear")
    for suffix, blob_name in ((".incomplete.bin","ps_b1"),(".pending.bin","rtv_after")):
        bad = read_snapshot(str(path)+suffix)
        bad_report = verify_snapshot(bad)
        require(len(bad.draws) == 1 and not bad_report["draws"][0]["replay_inputs_complete"], "incomplete fixture mislabeled")
        require(not bad.draws[0].blobs[blob_name].meta["complete"] and not bad.draws[0].blobs[blob_name].payload,
                "missing data became valid black")
        require(all(b.meta["complete"] for name,b in bad.draws[0].blobs.items() if name != blob_name),
                "independent inputs lost on a partial capture")
    ranged = verify_snapshot(read_snapshot(str(path)+".range.bin"))["draws"][0]
    require(ranged["inputs_complete"] and not ranged["geometry_verified"], "out-of-window index wrongly accepted")
    return report


def self_test():
    def must_fail(fn):
        try: fn()
        except (PanelSnapshotError,UnicodeDecodeError,json.JSONDecodeError): return
        raise AssertionError("invalid evidence accepted")
    raw = MAGIC+struct.pack("<7I2Q",1,0,0,0,0,0,0,0,0)+struct.pack("<I",0)
    assert read_bytes(raw).draws == []
    for data in (raw[:-1],raw+b"x",b"badmagic"+raw[8:]): must_fail(lambda:read_bytes(data))
    unavailable = Blob("ps_t0",{"type":"texture","bytes":0,"complete":False,"reason":"absent"},memoryview(b""))
    verify_blob(unavailable)
    must_fail(lambda:verify_blob(dataclasses.replace(unavailable,payload=memoryview(b"\0"))))
    ib = Blob("ib",{"complete":True,"format":57,"offset":10,"binding_offset":4},memoryview(struct.pack("<3H",0,7,2)))
    vb = Blob("vb0",{"complete":True,"stride":16,"binding_offset":8,"offset":24,"inputclass":0,"step":0},memoryview(bytes(128)))
    inst = Blob("vb1",{"complete":True,"stride":16,"binding_offset":4,"offset":52,"inputclass":1,"step":2},memoryview(bytes(32)))
    draw = Draw({"count":3,"instances":4,"start":3,"base":1,"start_instance":3,"kind":ord('X'),"topology":4,
                 "layout":[["P",0,6,0,0,0,0],["I",0,16,1,4,1,2]]},{"ib":ib,"vb0":vb,"vb1":inst})
    assert verify_geometry(draw)
    # Interior index 7 is the maximum: checking only the last index is wrong.
    draw.blobs["vb0"] = dataclasses.replace(vb,payload=memoryview(bytes(64)))
    must_fail(lambda:verify_geometry(draw))
    draw.blobs["vb0"] = vb
    draw.blobs["vb1"] = dataclasses.replace(inst,payload=memoryview(bytes(20)))
    must_fail(lambda:verify_geometry(draw))
    draw.blobs["vb1"] = inst
    draw.info["base"] = -1
    must_fail(lambda:verify_geometry(draw))
    bc = {"type":"texture","bytes":56,"complete":True,"reason":"","resource":1,
          "resource_desc":[8,8,4,1,70,1,0,0,8,0,0],"storage_format":71,"origin":[0,0],
          "view":[71,4,0,0xffffffff,0,0],"view_first_mip":0,"view_mips":4,"mips":[]}
    offset = 0
    for level,(w,row,rows) in enumerate(((8,16,2),(4,8,1),(2,8,1),(1,8,1))):
        bc["mips"].append(dict(level=level,width=w,height=w,row_bytes=row,rows=rows,offset=offset,size=row*rows))
        offset += row*rows
    verify_blob(Blob("ps_t1",bc,memoryview(bytes(56))))
    bc["mips"][2]["row_bytes"] = 4
    must_fail(lambda:verify_blob(Blob("ps_t1",bc,memoryview(bytes(56)))))
    print("eye_panel_snapshot self-test: PASS (framing, missing data, indexed/instance bounds, BC1 mip packing)")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path",type=Path,nargs="?")
    parser.add_argument("--self-test",action="store_true")
    parser.add_argument("--verify-fixture",action="store_true")
    args = parser.parse_args()
    try:
        if args.self_test: self_test();return 0
        if args.path is None: parser.error("a snapshot path is required")
        report = verify_fixture(args.path) if args.verify_fixture else verify_snapshot(read_snapshot(args.path))
        print(json.dumps(report,indent=2));return 0
    except (OSError,ValueError,KeyError,TypeError) as e:
        print(f"eye_panel_snapshot: FAIL: {e}");return 1


if __name__ == "__main__":
    raise SystemExit(main())

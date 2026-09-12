"""Build differential GPU fixtures for the production motion-vector shader.

Run from the repository root. The reference substitutes a scalar depth
neighbourhood; all motion/UI calculations remain production code. Fixtures
exercise depth layering, borders, cropped submissions and history modes.
The C++ runner compares every byte of all four reconstruction inputs.
"""
from pathlib import Path
import re, struct, json
import numpy as np

root=Path(__file__).resolve().parents[2]
out=root/'build/motion_kernel_test';out.mkdir(parents=True,exist_ok=True)
source=(root/'src/d3d11/temporal_shader_source.h').read_text()
part=source[source.index('constexpr char kTemporalCsHlsl'):]
hlsl=''.join(re.findall(r'R"HLSL\((.*?)\)HLSL"',part,re.S))
reference=hlsl
first=reference.index('    int2 local =',reference.index('void mv('))
last=reference.index('    GroupMemoryBarrierWithGroupSync();',first)+len('    GroupMemoryBarrierWithGroupSync();')
reference=reference[:first]+reference[last:]
reference=reference.replace('mvDepthTile[(local.y + 1) * 10 + local.x + 1]','zSceneAt(region.xy + int2(p))')
reference=reference.replace('mvDepthTile[(local.y + 1 + oy) * 10 + local.x + 1 + ox]',
                          'zSceneAt(region.xy + clamp(int2(p) + int2(ox, oy), int2(0,0), size-1))')
assert 'local.' not in reference
for name,code in [('reference',reference),('diagnostic',hlsl),('normal','#define EDVR_TEMPORAL_DIAGNOSTICS 0\n'+hlsl)]:
 (out/(name+'.hlsl')).write_text(code)
cb=re.search(r'cbuffer P.*?\{(.*?)\};',hlsl,re.S)[1]
cb=re.sub(r'//[^\n]*','',cb);offsets={};template=bytearray()
for typ,name,n in re.findall(r'\b(int4|int2|float4|float|int)\s+(\w+)\s*(?:\[(\d+)\])?\s*;',cb):
 count=int(n or 1)*({'int4':4,'int2':2,'float4':4}.get(typ,1));offsets[name]=(len(template),count,typ.startswith('int'));template.extend(bytes(4*count))
for case in range(20):
 w,h=[(1,1),(8,8),(9,7),(137,91),(131,127)][case%5]
 ox,oy=(7,11) if case%2 else (0,0);tw,th=w+ox+5,h+oy+3
 ow,oh=w*2,h*2
 d=out/f'{case:02}';d.mkdir(exist_ok=True)
 buf=template.copy()
 def put(name,values):
  off,count,integer=offsets[name];values=np.array(values).flatten().tolist()
  assert len(values)==count,(name,count,len(values))
  struct.pack_into('<'+('i' if integer else 'f')*count,buf,off,*values)
 put('region',[ox,oy,ox+w,oy+h]);put('size',[w,h]);put('texSize',[tw,th])
 put('tanNow',[-1.5,1,-1.2,1.2]);put('tanPrev',[-1.5,1,-1.2,1.2]);put('jit',[-.375,.22,1,.5])
 for prefix in ['dR','c2R','stR','st2R','wR']:
  for i in range(3):
   name=prefix+str(i)
   if name in offsets:put(name,[int(i==0),int(i==1),int(i==2),0])
 put('tvUsed',[.015,-.008,.002,1]);put('tvCam',[.12,.03,-.07,case%3!=0])
 put('tvSt',[.5,-.1,.02,1]);put('tv2St',[-.25,.05,0,1]);put('knobs',[0,case!=18,.025,50000])
 put('split',[10,0,0,0]);put('objects',[1500,30,900,0]);put('probe',[2,1,1,6])
 put('box0',[-1024,-1024,-1024,0]);put('box1',[1024,1024,1024,0])
 # Previous-depth disagreement exercises the optional mover rejection path.
 put('movers',[case%2,1,.65,0])
 if case>=15:
  put('tvSt',[0,0,0,0]);put('ships',[1,0,0,0])
  for name,shape,first in [('shR',(24,4),[[1,0,0,0],[0,1,0,0],[0,0,1,0]]),
                           ('shTv',(8,4),[[.5,-.2,0,1]]),
                           ('shBox0',(8,4),[[-1024,-1024,-1024,1]]),
                           ('shBox1',(8,4),[[1024,1024,1024,0]]),
                           ('shParts',(256,4),[[0,0,50,0]]),
                           ('shRect',(8,4),[[0,0,w,h]])]:
   a=np.zeros(shape);a[:len(first)]=first;put(name,a)
  put('objects',[1500,30,1e7,0])
 rng=np.random.default_rng(7141+case)
 colour=rng.integers(0,256,(th,tw,4),dtype=np.uint8);colour[...,3]=255;colour.tofile(d/'colour.bin')
 rng.integers(0,256,(oh,ow,4),dtype=np.uint8).tofile(d/'history.bin')
 for k in ['depth','smoke','ui_depth']:
  z=rng.choice([0,0,0,.000001,.0005,.0025,.025,.08],(th,tw)).astype('<f4')
  if case>=10:
   # Coherent far geometry must actually exercise body/ship transforms;
   # per-pixel random near depths would dominate almost every 3x3.
   yy,xx=np.indices((th,tw));z=np.where((xx//17+yy//13)%3,.0005,0).astype('<f4')
  if k!='depth' and case<5:z.fill(0)
  z.tofile(d/(k+'.bin'))
 rng.choice([0,.0004,.003,.03],(h,w)).astype('<f4').tofile(d/'previous_depth.bin')
 rng.choice([0,1,2,3,65,66,131,253,254],(h,w)).astype('u1').tofile(d/'mask.bin')
 prev=rng.integers(0,256,(h,w,4),dtype=np.uint8);prev[...,3]=rng.choice([0,255],(h,w));prev.tofile(d/'ui_previous.bin')
 (d/'params.bin').write_bytes(buf);(d/'size.bin').write_bytes(struct.pack('<6I',w,h,ow,oh,tw,th))
print(f'Prepared 20 fixtures in {out}; constant buffer {len(template)} bytes.')

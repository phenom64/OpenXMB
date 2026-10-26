from pathlib import Path
import struct
import argparse

parser=argparse.ArgumentParser(description='Generate synthetic Edge/GTF/VSMX regression resources.')
parser.add_argument('output',type=Path,help='Directory for generated raw RAF resources')
out=parser.parse_args().output
out.mkdir(parents=True,exist_ok=True)
def put(b,o,f,*v): struct.pack_into('>'+f,b,o,*v)
geometry=bytearray(0x1d0)
put(geometry,0,'II',1,8)
s=8
put(geometry,s+8,'HH',4,6)
put(geometry,s+0x10,'I',0x140)
put(geometry,s+0x14,'H',10)
put(geometry,s+0x18,'I',0x100)
put(geometry,s+0x2c,'I',0x180)
put(geometry,s+0x74,'I',0x1c0)
for i,p in enumerate([(-5.333,-3,0),(5.333,-3,0),(5.333,3,0),(-5.333,3,0)]):put(geometry,0x100+i*12,'fff',*p)
geometry[0x140:0x14a]=bytes([0,0,0,0,0,1,0,0,0,0xc0])
for i,uv in enumerate([(0,1),(1,1),(1,0),(0,0)]):put(geometry,0x180+i*8,'ff',*uv)
geometry[0x1c0:0x1d0]=bytes([1,8,0,0,0,0,0,0,0,2,2,5,0,0,0,0])
(out/'raf-geometry.bin').write_bytes(geometry)
w,h=128,72
texture=bytearray(52+w*h*4)
put(texture,16,'II',52,w*h*4)
texture[24]=0xa5
put(texture,32,'HH',w,h)
put(texture,40,'I',w*4)
for y in range(h):
 for x in range(w): texture[52+(y*w+x)*4:56+(y*w+x)*4]=bytes([255,20+x,35+y,110+x//2])
(out/'raf-texture.gtf').write_bytes(texture)
code=[];names=[];props=[];texts=[]
def emit(op,arg=0):code.append([op,arg]);return len(code)-1
def table(items,s):
 if s not in items:items.append(s)
 return items.index(s)
def name(s):emit(0x2e,table(names,s))
def prop(s):emit(0x2f,table(props,s))
def method(s):emit(0x30,table(props,s))
def number(n):emit(0x27,struct.unpack('<I',struct.pack('<f',n))[0])
name('actor');name('Actor');emit(0x28,table(texts,'sun'));emit(0x3e,1);emit(1);emit(0x22)
name('phase');number(0);emit(1);emit(0x22)
name('actor');prop('timer');emit(0x26,0);name('IntervalTimer');number(1/30);fn=emit(0x2a);emit(0x3e,2);emit(0x36);emit(0x22);emit(0x45)
code[fn][1]=len(code)
emit(0x2c);method('setPosition');name('Math');method('sin');name('phase');name('phase');number(.04);emit(2);emit(1);emit(0x3d,1);number(0);number(0);emit(0x49,3);number(0);emit(0x3d,2);emit(0x22);emit(0x45)
script=bytearray(48);script[:4]=b'VSMX'
def le(o,n):struct.pack_into('<I',script,o,n)
le(8,len(script));le(12,len(code)*8)
for op,arg in code:script.extend(struct.pack('<II',op,arg))
for header,items in [(16,texts),(28,props),(40,names)]:
 begin=len(script)
 for s in items:script.extend(s.encode()+b'\0')
 le(header,begin);le(header+4,len(script)-begin)
(out/'raf-script.vsmx').write_bytes(script)
print('Wrote RAF Edge geometry, ARGB texture and timer-driven VSMX fixture',len(geometry),len(texture),len(script))

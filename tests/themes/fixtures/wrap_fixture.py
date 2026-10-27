from pathlib import Path
import struct,zlib,sys
root=Path(sys.argv[1]) if len(sys.argv)>1 else Path(__file__).parent

def resource_tree(magic,children,compressed=False):
    strings=bytearray();strings_index={};integers=bytearray();floats=bytearray();files=bytearray()
    def text(value):
        if value not in strings_index:
            strings_index[value]=len(strings);strings.extend(value.encode('utf-8')+b'\0')
        return strings_index[value]
    nodes=[('theme' if magic==b'P3TF' else 'raf',{})]+children
    offsets=[];size=0
    for name,attrs in nodes:offsets.append(size);size+=28+16*len(attrs)
    tree=bytearray(size)
    for i,(name,attrs) in enumerate(nodes):
        at=offsets[i]
        struct.pack_into('>IIIIIII',tree,at,text(name),len(attrs),0xffffffff,0xffffffff,offsets[i+1] if i and i+1<len(nodes) else 0xffffffff,offsets[1] if i==0 and len(nodes)>1 else 0xffffffff,0xffffffff)
        for j,(key,value) in enumerate(attrs.items()):
            if isinstance(value,bytes):
                kind=6;data=zlib.compress(value) if compressed else value;v1=len(files);v2=len(data);files.extend(data)
            elif isinstance(value,str):kind=3;v1=text(value);v2=0
            elif isinstance(value,int):kind=1;v1=value&0xffffffff;v2=0
            elif isinstance(value,float):kind=2;v1=struct.unpack('>I',struct.pack('>f',value))[0];v2=0
            elif isinstance(value,list):
                is_integer=all(isinstance(x,int) for x in value);kind=4 if is_integer else 5;table=integers if is_integer else floats
                v1=len(table)//4;v2=len(value)
                for number in value:table.extend(struct.pack('>i' if is_integer else '>f',number))
            else:raise ValueError(value)
            struct.pack_into('>IIII',tree,at+28+16*j,text(key),kind,v1,v2)
    sections=[tree,bytearray(),strings,integers,floats,files]
    header=bytearray(magic+struct.pack('>I',0x00000110))
    at=56
    for section in sections:header.extend(struct.pack('>II',at,len(section)));at+=len(section)
    assert len(header)==56
    return bytes(header)+b''.join(sections)

def png():
    def chunk(kind,data):return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data))
    rows=bytearray()
    for y in range(72):
        rows.append(0)
        for x in range(128):rows.extend((12+x//4,22+y//2,65+x//2,255))
    return b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',128,72,8,6,0,0,0))+chunk(b'IDAT',zlib.compress(rows))+chunk(b'IEND',b'')

raf=resource_tree(b'RAFO',[
 ('file',{'id':'texture-file','src':(root/'raf-texture.gtf').read_bytes()}),
 ('file',{'id':'geometry-file','src':(root/'raf-geometry.bin').read_bytes()}),
 ('file',{'id':'script-file','src':(root/'raf-script.vsmx').read_bytes()}),
 ('texture',{'id':'mainTexture','type':0,'fileindex':0}),
 ('material',{'id':'mainMaterial','effect':'pure_texture_depth_0'}),
 ('_texture',{'texref':'mainTexture'}),
 ('model',{'id':'mainModel'}),
 ('geometry',{'id':'quad','fileindex':1}),
 ('camera',{'id':'camera','type':0,'yfov':0.6,'znear':0.1,'zfar':1000.0,'position':[0,0,10],'direction':[0,0,-1],'up':[0,1,0]}),
 ('actor',{'id':'sun','model':'mainModel','material':'mainMaterial','position':[0,0,0],'rotation':[0,0,0],'scale':[1,1,1],'color':[1,1,1,1]}),
 ('script',{'id':'main','fileindex':2})
])
(root/'native-scripted.raf').write_bytes(raf)
p3t=resource_tree(b'P3TF',[
 ('info',{'name':'Native Animated Test','author':'OpenXMB test fixture','comment':'Generated texture and Edge geometry; native VSMX sine timer.','preview':png()}),
 ('bgimage',{'hd':png(),'anim':b'_RAF'+struct.pack('>I',len(raf))+zlib.compress(raf)})
],compressed=True)
destination=Path(sys.argv[2]) if len(sys.argv)>2 else root/'native-scripted.p3t'
destination.write_bytes(p3t)
print(f'Wrote {len(p3t)} byte P3TF containing {len(raf)} byte scripted RAFO scene')

import struct, zlib, os, sys, json

class Rec:
    __slots__=("sig","formid","flags","subs")
    def __init__(s,sig,formid,flags,subs): s.sig,s.formid,s.flags,s.subs=sig,formid,flags,subs
    def get(s,t):
        for a,b in s.subs:
            if a==t: return b
        return None
    def all(s,t): return [b for a,b in s.subs if a==t]

def parse_subs(data):
    subs=[]; i=0; n=len(data); pending=None
    while i+6<=n:
        t=data[i:i+4].decode('latin-1'); sz=struct.unpack('<H',data[i+4:i+6])[0]; i+=6
        if t=='XXXX':
            pending=struct.unpack('<I',data[i:i+4])[0]; i+=4; continue
        if pending is not None: sz=pending; pending=None
        subs.append((t,data[i:i+sz])); i+=sz
    return subs

def scan(path, wanted):
    out={w:[] for w in wanted}
    with open(path,'rb') as f: buf=f.read()
    n=len(buf)
    def walk(off,end):
        i=off
        while i+24<=end:
            sig=buf[i:i+4].decode('latin-1')
            if sig=='GRUP':
                gsz=struct.unpack('<I',buf[i+4:i+8])[0]
                label=buf[i+8:i+12]; gtype=struct.unpack('<i',buf[i+12:i+16])[0]
                if gtype==0:
                    lbl=label.decode('latin-1')
                    if lbl in wanted or lbl in ('CELL','WRLD'):
                        walk(i+24, i+gsz)
                else:
                    walk(i+24,i+gsz)
                i+=gsz
            else:
                dsz=struct.unpack('<I',buf[i+4:i+8])[0]
                flags=struct.unpack('<I',buf[i+8:i+12])[0]
                fid=struct.unpack('<I',buf[i+12:i+16])[0]
                if sig in wanted:
                    d=buf[i+24:i+24+dsz]
                    if flags & 0x00040000:
                        try: d=zlib.decompress(d[4:])
                        except Exception: d=b''
                    out[sig].append(Rec(sig,fid,flags,parse_subs(d)))
                i+=24+dsz
    # skip TES4
    hs=struct.unpack('<I',buf[4:8])[0]
    walk(24+hs, n)
    return out

def zs(b):
    if b is None: return None
    return b.split(b'\x00')[0].decode('latin-1')

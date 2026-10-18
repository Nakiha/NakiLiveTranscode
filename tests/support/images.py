"""Small deterministic image fixtures."""
import binascii
import struct
import zlib

def png(path,primaries=None):
    def chunk(kind,data):
        return struct.pack(">I",len(data))+kind+data+struct.pack(">I",binascii.crc32(kind+data)&0xffffffff)
    data=b"\x89PNG\r\n\x1a\n"+chunk(b"IHDR",struct.pack(">IIBBBBB",8,8,8,6,0,0,0))
    if primaries:data+=chunk(b"cHRM",struct.pack(">8I",*primaries))
    data+=chunk(b"IDAT",zlib.compress((b"\0"+bytes([255,0,0,255])*8)*8))+chunk(b"IEND",b"")
    path.write_bytes(data)

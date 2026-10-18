#!/usr/bin/env python3
"""Independent source-pinned format golden; no catalog code or fixture imports."""
import hashlib, json, pathlib, struct
out = pathlib.Path(__file__).parent
# Literal source-audited offsets: header 0x20, Patch ext 0x18,
# PackagedContentInfo 0x38, ContentMetaInfo 0x10, digest 0x20.
raw = bytes.fromhex('''
00583412000000014433221181001800
01000100000000000000000000000000
00503412000000010000000000000000
00000000000000000001020304050607
08090a0b0c0d0e0f1011121314151617
18191a1b1c1d1e1fa0a1a2a3a4a5a6a7
a8a9aaabacadaeaf7856341200000107
1122334455667788d4c3b2a182040000
''')
assert len(raw) == 0x80
cnmt = raw + hashlib.sha256(raw).digest()
(out/'patch-sentinel.cnmt').write_bytes(cnmt)
# Same wire bytes have two defined interpretations; the policy must be explicit.
variant = bytearray(raw)
variant[0x6c] = 0x34
variant[0x6d] = 0xa5
variant = bytes(variant)
(out/'layout-sentinel.cnmt').write_bytes(variant+hashlib.sha256(variant).digest())
# One real PFS0, literal metadata base 0x40 with 0x18 bytes of string table.
pfs = bytearray(0x200)
struct.pack_into('<4sIII',pfs,0,b'PFS0',1,0x18,0)
struct.pack_into('<QQII',pfs,0x10,0,len(cnmt),0,0)
pfs[0x28:0x35]=b'fixture.cnmt\0'
pfs[0x40:0x40+len(cnmt)]=cnmt
# Plain unsigned NCA3 with one genuine two-layer SHA256 PFS section.
nca = bytearray(0x1000)
nca[0x200:0x204] = b'NCA3'
nca[0x204] = 0
nca[0x205] = 1 # NcaContentType::Meta
struct.pack_into('<QQII',nca,0x208,len(nca),0x0100000012345800,0,0x000f0000)
struct.pack_into('<IIII',nca,0x240,6,8,0,0)
fsh = bytearray(0x200)
struct.pack_into('<HBBBB',fsh,0,2,1,2,1,0) # version, PartitionFs, SHA256, None, no extra metadata hash
hash_table = hashlib.sha256(pfs).digest()
fsh[8:0x28] = hashlib.sha256(hash_table).digest()
struct.pack_into('<II',fsh,0x28,0x200,2)
struct.pack_into('<QQQQ',fsh,0x30,0,0x20,0x200,0x200)
nca[0x400:0x600] = fsh
nca[0x280:0x2a0] = hashlib.sha256(fsh).digest()
nca[0xc00:0xc20] = hash_table
nca[0xe00:0x1000] = pfs
name = hashlib.sha256(nca).digest()[:16].hex()+'.cnmt.nca'
(out/name).write_bytes(nca)
# Outer PFS0: one name, metadata rounded to 0x20 through declared string-table padding.
names = name.encode()+b'\0'
string_size = (len(names)+0x28+0x1f)//0x20*0x20-0x28
header = struct.pack('<4sIIIQQII',b'PFS0',1,string_size,0,0,len(nca),0,0)
outer = header+names+bytes(string_size-len(names))+nca
(out/'patch-sentinel.nsp').write_bytes(outer)
manifest = {
 'provenance': 'Independent static literal/source layout; imports no production reader or writer.',
 'cnmt': {'file':'patch-sentinel.cnmt','size':len(cnmt),'headerSize':32,'extendedHeaderSize':24,'contentCount':1,'contentMetaCount':1,'contentOffset':56,'contentMetaOffset':112,'extendedDataOffset':128,'digestOffset':128,
  'id':'0100000012345800','version':0x11223344,'role':'Patch','applicationId':'0100000012345000',
  'content':{'id':'a0a1a2a3a4a5a6a7a8a9aaabacadaeaf','size':0x12345678,'type':1,'idOffset':7},
  'contentMeta':{'id':'8877665544332211','version':0xa1b2c3d4,'type':0x82,'attributes':4}},
 'layoutVariant':{'file':'layout-sentinel.cnmt','rawSizeBytes':'7856341234a5','current40Size':0x3412345678,'currentAttributes':0xa5,'legacy48Size':0xa53412345678},
 'nca':{'file':name,'size':len(nca),'headerAuthentication':'unsigned/unverified','contentType':1,'sectionStart':0xc00,'sectionEnd':0x1000,'fsHeaderOffset':0x400,'fsType':1,'hashType':2,'encryptionType':1,'hashTableOffsetInSection':0,'hashTableSize':32,'dataOffsetInSection':0x200,'dataSize':0x200,'cnmtAbsoluteOffset':0xe40},
 'nsp':{'file':'patch-sentinel.nsp','size':len(outer),'dataOffset':len(outer)-len(nca)},
 'expectedCatalog':{'groupCount':1,'role':'Patch','runnable':False,'missingReferencedContent':True,'contentMetaInfoIsNotContentHash':True},
 'sha256':{}
}
for path in sorted(out.iterdir()):
 if path.suffix in ['.cnmt','.nca','.nsp']:
  manifest['sha256'][path.name] = hashlib.sha256(path.read_bytes()).hexdigest()
(out/'expected.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(json.dumps(manifest,indent=2))

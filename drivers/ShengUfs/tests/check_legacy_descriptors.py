#!/usr/bin/env python3
"""Compare actual adoptAndRead builders with current root descriptors offline.

Extract only the historical builder prefixes, stopping before cache/MMIO.
They are comparison oracles, never a fallback to commandeer firmware RAM.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',required=True,type=Path)
    parser.add_argument('--compiler',default='clang++')
    args=parser.parse_args()
    source=Path(__file__).resolve().parent
    out=args.output.resolve(); out.mkdir(parents=True,exist_ok=True)
    production=(source.parent/'NativeUfs.cpp').read_text()
    legacy=production[production.index('void NativeUfs::adoptAndRead ('):]
    prefixes=[]
    for name in ('query','command'):
        start=legacy.index('auto '+name+' = [&]')
        end=legacy.index('    cacheClean (virt, 16384);',start)
        prefixes.append(legacy[start:end]+('return 0;' if name=='query' else 'return UfsResult{};')+'\n};\n')
    fixture=r'''
#include "RootCommandsBridge.cpp"
#include <array>
static std::array<uint8_t,kDmaBytes> sent;
static uint64_t ticks;
static uint64_t io(unsigned op,uint64_t address,uint64_t,void *) {
  if (op==1 && address==0x58) {
    memcpy(sent.data(),memory,kDmaBytes);
    *reinterpret_cast<uint32_t *>(memory+8)=0;
    uint8_t *c=memory+kUcdOffset,*r=c+kRspOffset;
    memcpy(r,c,32); r[0]=c[0]+0x20;
  }
  if (op==4) return ++ticks;
  return 0;
}
static void zero32(void *p,unsigned length) { memset(p,0,length); }
static void be32(uint8_t *p,uint32_t value) { putBe32(p,value); }
struct UfsResult {};
static unsigned cases;
static void same(const uint8_t *old) {
  assert(!memcmp(sent.data(),old,32));
  assert(!memcmp(sent.data()+kUcdOffset,old+kUcdOffset,kPrdtOffset+16));
  ++cases;
}
int main() {
  alignas(4096) uint8_t old[kDmaBytes];
  uint8_t *virt=old;
  /* The extracted builder prefixes below use the driver-s own local names; they are aliases of
   * RootTransportLayout.h-s constants (reached through RootCommandsBridge.cpp), not a second copy of the
   * numbers, so a change to the layout moves this fixture with the driver instead of leaving it behind. */
  const uint32_t ucdOffset=kUcdOffset,rspOffset=kRspOffset,prdtOffset=kPrdtOffset,dataOffset=kDataOffset;
  static_assert(sizeof old==kDmaBytes,"the fixture-s buffer is the driver-s window");
  for (uint64_t phys: {0x40010000ULL,0x8cb5e9000ULL}) {
    // PREFIXES
    uint32_t ignored=0;
    memset(old,0,sizeof(old)); ufs_test_reset(io,phys);
    (void)query(0,0,0,&ignored,&ignored); (void)ufs_test_nop(); same(old);
    for (uint8_t opcode: {3,5}) {
      memset(old,0,sizeof(old)); ufs_test_reset(io,phys);
      (void)query(1,opcode,2,&ignored,&ignored);
      (void)ufs_test_query(opcode,2,&ignored); same(old);
    }
    for (uint8_t lun: {0,0xd0}) {
      uint8_t cdb[10]={0x28,0,0,0,0,8,0,0,1,0};
      memset(old,0,sizeof(old)); ufs_test_reset(io,phys);
      (void)command(lun,cdb,10,true,4096);
      (void)ufs_test_scsi(lun,cdb,10,true,4096,&ignored); same(old);
      cdb[0]=0x1b; cdb[3]=0x10;
      memset(old,0,sizeof(old)); ufs_test_reset(io,phys);
      (void)command(lun,cdb,6,false,0);
      (void)ufs_test_scsi(lun,cdb,6,false,0,&ignored); same(old);
    }
  }
  assert(cases==14); printf("PASS %u legacy/current descriptor comparisons\n",cases);
}
'''.replace('// PREFIXES','\n'.join(prefixes))
    (out/'compare.cpp').write_text(fixture)
    subprocess.run([args.compiler,'-std=c++17','-O2','-fsanitize=address,undefined',
                    '-fno-sanitize-recover=undefined',
                    '-fno-omit-frame-pointer','-I',str(source),str(out/'compare.cpp'),
                    '-o',str(out/'compare')],check=True)
    # The report said passed: true whatever happened, and a recoverable UBSan report would have left the run
    # successful. A sanitizer diagnostic is a failure now, and the verdict comes from the run itself.
    ran=subprocess.run([str(out/'compare')],text=True,timeout=10,capture_output=True)
    log=ran.stdout+ran.stderr
    (out/'result.log').write_text(log); print(log,end='')
    sanitizer=[m for m in ('runtime error:','ERROR: AddressSanitizer') if m in log]
    if ran.returncode or sanitizer:
        raise SystemExit(f'legacy descriptor comparison failed (exit {ran.returncode}, markers {sanitizer}); see {out/"result.log"}')
    # passed is true because the run above exited 0 without a sanitizer diagnostic and printed its PASS line.
    report={'cases':14,'passed':True,'scope':'UTRD 32 bytes and UCD/response/PRDT 1040 bytes before doorbell; '
            'does not compare allocation, coherence, queue takeover or controller behavior',
            'source_sha256':{name:hashlib.sha256((source.parent/name).read_bytes()).hexdigest()
                for name in ('NativeUfs.cpp','RootCommands.h','RootTransportLayout.h','tests/RootCommandsBridge.cpp')},
            'extracted_prefix_sha256':[hashlib.sha256(p.encode()).hexdigest() for p in prefixes]}
    (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')


if __name__=='__main__':
    main()

#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Bind an owned concrete backend to the unchanged original Legacy lifecycle."""
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'build/native-legacy-display';OUT.mkdir(parents=True,exist_ok=True)
sys.path.insert(0,str(ROOT/'kits/XnuPrelink'))
from native_prelink import macho,backed_offset,exports
def run(cmd,**kw):return subprocess.check_output([str(x) for x in cmd],timeout=90,**kw)
def owner_data_uuid():
    """The Data volume UUID package/KeyStoreProbe.cpp hashes into the key store's device key.

    It is a build input already: `unixnu defconfig` writes CONFIG_XNU_OWNER_DATA_VOLUME_UUID into the
    generated .config, which sits beside the tree this script runs in, and the kernel script turns it
    into build/xnu/owner-data-volume.h. Reading the same config here keeps one source for both.
    """
    config=ROOT.parent/'.config'
    require(config.is_file(),f'{config} is missing: run unixnu defconfig before building the kernel')
    for line in config.read_text().splitlines():
        key,_,value=line.partition('=')
        if key.strip()=='CONFIG_XNU_OWNER_DATA_VOLUME_UUID':return value.strip().strip('"')
    raise SystemExit(f'{config} has no CONFIG_XNU_OWNER_DATA_VOLUME_UUID')
def require(v,why):
    if not v:raise SystemExit(why)
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()

# A clean build has no prior prelinked candidate. Emit a temporary archive so the first
# kernel can link and become that candidate; the next build runs the audited path below.
candidate=ROOT/'build/xnu-prelink/native-owner-mobile-graphics-candidate.macho'
if not candidate.exists() and not any((ROOT/'build/xnu-prelink').glob('native-owner-ufs-cpu-desktop-*-candidate.macho')):
    OUT.mkdir(parents=True,exist_ok=True)
    stub=OUT/'FirstPassStub.c'
    stub.write_text('// First cold-build pass; replaced after the candidate kernel is available.\n'
                    'void native_legacy_count_exec(void);\n'
                    'void native_legacy_note_exit(void);\n'
                    'void native_legacy_paint_userspace_band(void);\n'
                    'void native_legacy_count_exec(void) {}\n'
                    'void native_legacy_note_exit(void) {}\n'
                    'void native_legacy_paint_userspace_band(void) {}\n')
    run(['xcrun','--sdk','iphoneos','clang','-arch','arm64','-mkernel',
         '-miphoneos-version-min=17.0','-Os','-c',stub,'-o',OUT/'FirstPassStub.o'])
    for archive in ('libNativeLegacyDisplay.late.a','libNativeLegacyDisplay.a'):
        path=OUT/archive
        path.unlink(missing_ok=True)
        run(['xcrun','ar','rcs',path,OUT/'FirstPassStub.o'])
    (OUT/'build-report.json').write_text(json.dumps(dict(
        first_pass_stubs=True,
        source_sha256={str(stub.relative_to(ROOT)):sha(stub)},
        archive_sha256=sha(OUT/'libNativeLegacyDisplay.late.a'),
        original_instructions_modified=False,driver_initialized=False,display_presented=False,
        hardware_execution=False),indent=2)+'\n')
    print('legacy display: no reference candidate yet; linked first-pass stubs; rebuild to bind the audited backend.')
    raise SystemExit(0)

print(run(['python3',ROOT/'scripts/audit-mobile-graphics-legacy.py'],text=True),end='')
audit=json.loads((ROOT/'build/display-analysis/mobile-graphics-legacy-abi.json').read_text())
pure=audit['abstract_slots'];methods=audit['methods']
header='''// SPDX-License-Identifier: BSD-2-Clause-Patent
// Opaque ABI storage for the exact original class, not source-level access
// to its private fields. The actual original metadata supplies its full
// Framebuffer/Service/IOService ancestry. Only qualified base prototypes may
// be called from source; opaque entries are original dispatch targets only.
#pragma once
#include <IOKit/IOService.h>
#include <libkern/libkern.h>
struct OriginalLegacySwapRequest;
class IOMobileFramebufferLegacy : public IOService {
 OSDeclareAbstractStructors(IOMobileFramebufferLegacy)
 unsigned char originalPrivate[0x18a0-sizeof(IOService)];
public:
 // Both fields are qualified by the complete original slot-249 getter.
 // This modifies this object's own mode state, never a vtable or executable.
 void installBootDimensions(uint32_t width,uint32_t height) {
  *reinterpret_cast<uint32_t *>(reinterpret_cast<unsigned char *>(this)+5808)=width;
  *reinterpret_cast<uint32_t *>(reinterpret_cast<unsigned char *>(this)+5812)=height;
 }
 const OriginalLegacySwapRequest *pendingSwapRequest() const {
  const OriginalLegacySwapRequest *request=nullptr;
  memcpy(&request,reinterpret_cast<const unsigned char *>(this)+5480,sizeof(request));return request;
 }
 // The real normal scanout backend removes its pending head before calling
 // slot 295. Slot 312 is a discard path (terminal state 8), not presentation.
 // The queue is a tail queue (head at +5480, tail pointer at +5488, each request's next at +2960 and previous-link at +2968): taking the head
 // moves the next request, if a client already queued one behind it (int-132: swap 3 arrived during swap 2's GPU copy and the old
 // single-entry form refused to retire swap 2, so every wait on it stayed busy), up to the head. The original queue's reference
 // moves to the current-swap slot for the original completion body.
 bool activateOnlyPendingSwap(const OriginalLegacySwapRequest *request) {
  auto self=reinterpret_cast<unsigned char *>(this);
  auto bytes=reinterpret_cast<const unsigned char *>(request);
  const void *current=nullptr,*head=nullptr,*tail=nullptr,*next=nullptr,*previous=nullptr;
  memcpy(&current,self+5472,8);memcpy(&head,self+5480,8);memcpy(&tail,self+5488,8);
  if(!request || current || head!=request)return false;
  memcpy(&next,bytes+2960,8);memcpy(&previous,bytes+2968,8);
  if(previous!=self+5480)return false;
  if(next) {
   const void *link=self+5480;
   memcpy(const_cast<unsigned char *>(reinterpret_cast<const unsigned char *>(next))+2968,&link,8);
  } else if(tail!=bytes+2960)return false;
  memcpy(self+5472,&request,8);
  if(next) {memcpy(self+5480,&next,8);}
  else {const void *empty=nullptr,*emptyTail=self+5480;memcpy(self+5480,&empty,8);memcpy(self+5488,&emptyTail,8);}
  return true;
 }
 void free() override;
 bool init(OSDictionary *properties=nullptr) override;
 IOReturn setProperties(OSObject *) override;
 void systemWillShutdown(IOOptionBits) override;
 IOService *probe(IOService *,SInt32 *) override;
 bool start(IOService *) override;
 void stop(IOService *) override;
 IOWorkLoop *getWorkLoop() const override;
 IOReturn newUserClient(task_t,void *,UInt32,IOUserClient **) override;
 IOReturn setPowerState(unsigned long,IOService *) override;
'''
for i in range(168,358):
    if i==184:header+=' virtual bool isMainDisplay();\n'
    elif i==188:header+=' virtual IOReturn getDotPitch(uint32_t *);\n'
    elif i==213:header+=' virtual IOReturn supportedFrameInfo(uint32_t,uint32_t *,char *,uint32_t *)=0;\n'
    elif i==249:header+=' virtual void getDisplayDimensions(uint32_t *,uint32_t *);\n'
    elif i==290:header+=' virtual void kickSwapQueue();\n'
    elif i==296:header+=' virtual bool validateSwap(const OriginalLegacySwapRequest *);\n'
    elif i==295:header+=' virtual void completeCurrentSwap();\n'
    elif i==337:header+=' virtual IOReturn collectCompletionReports(uint32_t,void *,uint32_t *,uint32_t)=0;\n'
    elif i==288:header+=' virtual bool startHardware();\n'
    elif i==301:header+=' virtual bool getRefreshAttributes(uint64_t *,bool);\n'
    elif i in pure:header+=f' virtual __attribute__((noreturn)) void opaque{i}()=0;\n'
    else:header+=f' virtual __attribute__((deprecated("Unclassified ABI: never call from source"))) void opaque{i}();\n'
header+='};\nstatic_assert(sizeof(IOMobileFramebufferLegacy)==0x18a0,"Actual original instance extent");\n'
(OUT/'OriginalLegacyFramebufferABI.h').write_text(header)
print(run(['python3',ROOT/'scripts/build-arm-driver.py','NativeLegacyDisplay'],text=True),end='')
obj=OUT/'NativeLegacyDisplay.o';raw=obj.read_bytes();image=macho(raw)
st=next(p for c,p,_ in image['commands'] if c==2);sy,count,strings,_=struct.unpack_from('<4I',raw,st+8);symbols=[]
for i in range(count):
    ix,kind,section,_,va=struct.unpack_from('<IBBHQ',raw,sy+i*16)
    symbols.append((raw[strings+ix:raw.index(b'\0',strings+ix)].decode(),va,kind))
vtable=next(va for name,va,_ in symbols if name=='__ZTV19NativeLegacyDisplay');relocations={}
for segment in image['segments']:
    for sec in segment['sections']:
        if sec['va']<=vtable<sec['va']+sec['size']:
            ro,rc=struct.unpack_from('<2I',raw,sec['command_offset']+56)
            for i in range(rc):
                offset,bits=struct.unpack_from('<2I',raw,ro+i*8)
                if bits>>27&1 and bits>>28==0 and bits>>25&3==3:relocations[sec['va']+offset]=symbols[bits&0xffffff][0]
compiled=[relocations.get(vtable+16+i*8) for i in range(358)]
require(all(compiled) and vtable+16+358*8 not in relocations,'Exact owned 358-entry vtable')
candidate=ROOT/'build/xnu-prelink/native-owner-mobile-graphics-candidate.macho'
if not candidate.exists():  # pruned: same fallback as scripts/audit-mobile-graphics-legacy.py (identical original family executable)
    later=sorted((ROOT/'build/xnu-prelink').glob('native-owner-ufs-cpu-desktop-*-candidate.macho'),key=lambda f:f.stat().st_mtime)
    require(later,'No reference candidate available');candidate=later[-1]
old=candidate.read_bytes();old_image=macho(old)
names={n:v for v,group in exports(old,old_image).items() for n in group}
def read(va,n):
    at=backed_offset(old_image['segments'],va,n,len(old));return old[at:at+n]
bindings={'__ZN25IOMobileFramebufferLegacyC2EPK11OSMetaClass':0xfffffff0057865cc,
          '__ZN25IOMobileFramebufferLegacyD2Ev':0xfffffff005786618,
          '__ZN25IOMobileFramebufferLegacy10gMetaClassE':0xfffffff006b35940,
          '__ZTV25IOMobileFramebufferLegacy':int(audit['legacy_vptr'],16)-16}
owned=[];native=[]
for i,name in enumerate(compiled):
    expected=int(methods[i]['address'],16)
    if name.startswith(('__ZN19NativeLegacyDisplay','__ZNK19NativeLegacyDisplay')):
        owned.append(i);require(i in (0,1,7,14,84,86,87,141,155,184,188,288,290,296,301) or i in pure,'Only qualified own methods and guarded leaves override')
    elif '25IOMobileFramebufferLegacy' in name:
        require(i not in pure,'No unresolved original abstract backend target');bindings[name]=expected
    else:
        require(i<168 and names.get(name)==expected,'Actual inherited native base symbol '+str(i));native.append(i)
require(owned==sorted([0,1,7,14,84,86,87,141,155,184,188,288,290,296,301]+pure),'Exact owned method set')
undefined={n for n,_,k in symbols if k==1 and 'IOMobileFramebufferLegacy' in n}
require(undefined==set(bindings),'All and only actual original-class imports qualified')
# Exact original IOSurface getters, independently observed through the real
# original swap request. These aliases call original code; no object layout
# is substituted and no original method/vtable is patched.
for name,address,field,wide in [
    ('descriptor',0xfffffff004b9ead0,48,True),('width',0xfffffff004b9ebbc,88,True),
    ('height',0xfffffff004b9ebc4,96,True),('stride',0xfffffff004b9ebd4,104,True),
    ('allocation',0xfffffff004b9ebfc,144,True),('format',0xfffffff004b9ebac,128,False),
    ('planes',0xfffffff004b9ebf4,152,False)]:
    expected=(0xf9400000 if wide else 0xb9400000)|((field//(8 if wide else 4))<<10)
    require(read(address,8)==struct.pack('<2I',expected,0xd65f03c0),'Exact original surface getter '+name)
    bindings['_native_legacy_surface_'+name]=address
# The original display-refresh notification sender (self, timestamp, continuous host time, frame interval): it sends the six-word
# vsync message (type 5) to every client entry queued at +5592, the tail queue that selector 72's registration (slot 208) fills at
# +5512 + type*16. The platform subclass that normally calls it on each panel vblank is not part of this port.
require(read(0xfffffff005796720,16)==bytes.fromhex('ff0302d1fd7b07a9fdc30191a0831ff8') and
        read(0xfffffff005796830,4)==bytes.fromhex('08ed4af9') and
        read(0xfffffff00579685c,16)==bytes.fromhex('e2230091c3008052240080521684ff97') and
        read(0xfffffff005795fbc,4)==bytes.fromhex('0ab182d2'),
        'Original vsync sender, its +5592 queue and six-word drop-allowed async send')
bindings['_native_legacy_vsync_notify']=0xfffffff005796720
# Slot 301 fills the six refresh words that setDisplayRefreshProperties publishes as IOMFBDisplayRefresh (minimum interval, its mach
# time, maximum interval, its mach time, step, its mach time; intervals in 32.32 seconds): the generic body fixes the minimum at
# 0x04444444 (1/60 s), and the render server paces its display timer by it.
require(int(methods[301]['address'],16)==0xfffffff00579ea5c and read(0xfffffff00579ea68,8)==bytes.fromhex('888888d28888a0f2') and
        read(0xfffffff00579ec54,4)==bytes.fromhex('08b544f9'),
        'Original slot-301 refresh words with the 1/60 s minimum, called by setDisplayRefreshProperties')
# setDisplayRefreshProperties itself (self): fills the six words through slot 301 and publishes them as IOMFBDisplayRefresh. Nothing in
# the family calls it (the platform subclass does), so without it the render server falls back to its own 1/60 s heartbeat.
require(read(0xfffffff00579ebb8,16)==bytes.fromhex('ff0302d1fd7b07a9fdc30191a0031ff8') and
        read(0xfffffff00579ed54,16)==bytes.fromhex('086540f90173ff90218c0c9100013fd6'),
        'Original setDisplayRefreshProperties publishing IOMFBDisplayRefresh through setProperty')
bindings['_native_legacy_set_refresh_properties']=0xfffffff00579ebb8
# Both protected destructors/constructors must follow the exact original chain.
def branch(pc):
    word=struct.unpack('<I',read(pc,4))[0];require(word&0xfc000000==0x94000000,'Actual protected base call')
    value=word&0x3ffffff;return pc+4*(value-(1<<26) if value&(1<<25) else value)
require(branch(0xfffffff0057865fc)==0xfffffff00577b510 and branch(0xfffffff005786630)==0xfffffff00577b55c,
        'Original protected Legacy C2/D2 really call their original Service bases')
# Actual caller passes only self at slot 288 and tests bit zero of w0.
require(read(0xfffffff005787ef8,16)==bytes.fromhex('080040f9088144f900013fd6e0000037'),
        'Original bool start_hardware() call and result check')
getter=int(methods[249]['address'],16)
for offset,field in [(0x30,5808),(0x58,5812)]:
    require(struct.unpack('<I',read(getter+offset,4))[0]==0xb9400000|((field//4)<<10)|(8<<5)|8,
            'Actual original width/height getter fields')
require(int(methods[295]['address'],16)==0xfffffff005795b88 and
        read(0xfffffff005795b98,16)==bytes.fromhex('e00740f9280080520101001272d3ff97'),
        'Actual original slot-295 passes true to the normal current completion body')
require(read(0xfffffff00578aa7c,24)==bytes.fromhex('080040f9084545f9030080d2e20303aa0400805200013fd6'),
        'Original completion calls optional report slot 337 with null outputs and zero limit')
require(read(0xfffffff005777e9c,16)==bytes.fromhex('080040f908e142f900013fd6c0000036'),
        'Original isMainDisplay dispatch uses only self and tests boolean bit zero')
asm=OUT/'original-bindings.S';asm.write_text(''.join(f'.globl {name}\n.set {name}, {va:#x}\n' for name,va in sorted(bindings.items())))
sdk=run(['xcrun','--sdk','iphoneos','--show-sdk-path'],text=True).strip()
run(['xcrun','clang','-target','arm64-apple-ios17.0','-isysroot',sdk,'-c',asm,'-o',OUT/'original-bindings.o'])
# The Mach-O aware llvm-objcopy comes from the launcher PATH (the toolchain shim directory), not from a
# Homebrew prefix: this tree is built on Linux, where `brew --prefix llvm` does not exist.
objcopy='llvm-objcopy';late=OUT/'NativeLegacyDisplay.late.o'
run([objcopy,'--remove-section=__DATA,__mod_init_func','--globalize-symbol=__GLOBAL__sub_I_NativeLegacyDisplay.cpp',obj,late])
def code(data,info):
    sec=next(s for g in info['segments'] for s in g['sections'] if s['name']=='__text');return data[sec['fileoff']:sec['fileoff']+sec['size']]
require(code(raw,image)==code(late.read_bytes(),macho(late.read_bytes())),'Only owned initializer section registration changes')
# The board probes (display, key store) are own-source files of this package. They are compiled from the
# same IOPlatformExpert record the generic driver used, with the warning set the official board script applies.
r=json.loads((ROOT/'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json').read_text().strip().rstrip(','))
cmd=[];args=r['arguments'];i=0;source=ROOT/'kits/dby_w09/LegacyDisplayProbe.cpp'
while i<len(args):
    value=args[i]
    if value==r['file']:cmd.append(str(source))
    elif value=='-o':cmd+=['-o',str(OUT/'LegacyDisplayProbe.o')];i+=1
    elif value=='-Xclang' and args[i+1].startswith('-W'):i+=1
    elif not value.startswith('-W'):cmd.append(value)
    i+=1
cmd+=['-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-unnecessary-virtual-specifier','-Wno-c99-designator','-Wno-reorder-init-list',
      '-Wno-deprecated-enum-enum-conversion','-I',str(ROOT/'.cache/xnu-work/osfmk')]
run(cmd,cwd=r['directory'])
probe=OUT/'LegacyDisplayProbe.o'
TOUCH=OUT/'touch';TOUCH.mkdir(exist_ok=True)
# The five files that became components on 2026-11-14 are compiled from drivers/<id>/ now; every TU that
# includes one of their headers needs the directory, and the two layouts (kit + component) are different.
component_include=['-I',str(ROOT/'drivers/ShengGpr'),
                   '-I',str(ROOT/'drivers/ShengAudioReach'),
                   '-I',str(ROOT/'drivers/ShengCs35l43'),
                   '-I',str(ROOT/'drivers/QcomGeniI2c')]
touch_include=['-I',str(ROOT/'kits/dby_w09')]+component_include
# HidInputProbe.cpp is the only place the owned IOHIDDevice is created and started: allocate the IOHIDDevice
# subclass, attach it to IOHIDResource, then start it, which is what brings up the 8 ms touch timer that turns
# native_touch_seq into HID reports. LinkStubs.c keeps a weak no-op native_hid_input_probe() for boards that do
# not select this component; without the real object here that no-op is what runs and no digitizer ever exists.
hid_cpp=ROOT/'kits/dby_w09/HidInputProbe.cpp';hid_cpp_obj=TOUCH/'HidInputProbe.o'
run([str(hid_cpp) if str(a)==str(source) else str(hid_cpp_obj) if str(a)==str(probe) else a for a in cmd]+touch_include,
    cwd=r['directory'])
# The IOHIDDevice subclass is component:ShengHidInput, listed ahead of this recipe in board.toml, so its build
# directory already holds the late-staged object (mod_init section removed, initialiser globalised). Carry that
# object here for the same reason the key-store stand-in below is carried: the kernel links in one pass, and a
# library listed before this archive has already been scanned by the time HidInputProbe's reference appears.
hid_drv_obj=ROOT/'build/sheng-hid-input/NativeHidInput.late.o'
# The class vtable is not compiled here: IOHIDFamily is a kext, so the component build pins IOHIDDevice's vtable to
# the original kernel address instead (__ZTV11IOHIDDevice, an absolute symbol). Without this object the link fails
# on the vtable of every NativeHidInput virtual.
hid_bindings_obj=ROOT/'build/sheng-hid-input/hid-original-bindings.o'
# The audio DSP bring-up (sheng-adsp=2) follows the kit block of upstream's build-gop-legacy-display.py:
# AdspProbe.cpp plus the protocol cores, the C ones compiled with the kernel osfmk C record. This board
# keeps its sources in package/, which the assembled tree exposes as kits/dby_w09. Upstream's SM8550-only
# units are not here: ShengBattmgr (the SM8550 fuel gauge) and MhiBoot (the PCIe modem) are not this
# machine's silicon, and neither is the SM8550 speaker half -- see the note above the archive list.
adsp_cpp=ROOT/'kits/dby_w09/AdspProbe.cpp';adsp_obj=TOUCH/'AdspProbe.o'
# QcomMdt.c and FatVolume.c are not compiled here: build-boot-progress.py already owns those two
# TUs for AdspFirmware.c, and both archives are force-loaded, so a second copy in this one is a
# duplicate symbol (_fat_mount/_fat_open/_fat_read and the qcom_mdt_* set). The ABI
# they provide is identical - it is the same package/ source - so boot-progress supplies it.
# ShengGpr, ShengAudioReach and ShengCs35l43 are component repositories since 2026-11-14: the assembled tree
# materialises them under drivers/<id>/, so this list reads them there and the include path below adds their
# directories. The rest stay in the board package.
COMPONENT_IDS=('ShengGpr','ShengAudioReach','ShengCs35l43')
adsp_c=[ROOT/'kits/dby_w09'/(n+'.c') for n in ('QcomQrtr','ShengQrtr','QcomSmem','QcomSmp2p',
        'QcomGlink','QcomQmp','QcomRpmh','ShengApr')]+[ROOT/'drivers'/n/(n+'.c') for n in COMPONENT_IDS]
# The speaker half of the SM8550 kit is deliberately not compiled or linked for this board. Its hardware
# layer is another machine's silicon: AudioProbe.cpp votes the SM8550 GCC register set, muxes SM8550 TLMM
# pins, addresses six CS35L43 amplifiers and calls the SM8550 button injector -- while this unit's own
# hardware answered as a TFA9874 quad on qupv3_se3 (0x98c000), whose wrap0 vote bits are proven as
# 6/7/8/9/13 at 0x52008 in package/DbySmartPA.cpp, sharing reset GPIO109, and whose ADSP is
# qcom,lpass@17300000 / qcom,pil-tz-generic, not the SM8550 PAS node. It was also unreachable at run
# time: sheng-audio is not in this board's kernel_args, so AudioProbe's entry returned at its first gate,
# while this kernel links no compiler_rt (___udivti3) and has no SM8550 button provider. The Kona graph
# is built from this machine's own vendor kernel instead, so nothing here is invented.
run([str(adsp_cpp) if str(a)==str(source) else str(adsp_obj) if str(a)==str(probe) else a for a in cmd]+touch_include,
    cwd=r['directory'])
# The protocol cores are C, so they do not take cmd: that record carries -xc++, which turns QcomMdt.c into
# three errors ((QcomMdtImage){0} and (QcomMdtSplit){0} under -Wmissing-field-initializers, and
# `uint8_t *out = to;` as a void* conversion C++ refuses). The official shape is upstream's kernel osfmk
# record: startup.o.json, with -mgeneral-regs-only and the .cache/xnu-work/osfmk include (upstream
# build-gop-legacy-display.py:212-237, applied to adsp_c at :291).
pr=json.loads((ROOT/'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/osfmk/DEVELOPMENT/startup.o.json').read_text().strip().rstrip(','))
core_cmd=[];pa=pr['arguments'];j=0
coresentinel=ROOT/'kits/dby_w09/QcomMdt.c';coreobjsentinel=TOUCH/'QcomMdt.o'
while j<len(pa):
    value=pa[j]
    if value==pr['file']:core_cmd.append(str(coresentinel))
    elif value in ('-o','-MF','-MT','-MJ'):
        if value=='-o':core_cmd+=['-o',str(coreobjsentinel)]
        j+=1
    elif value=='-Xclang' and pa[j+1].startswith('-W'):j+=1
    elif not value.startswith('-W'):core_cmd.append(value)
    j+=1
core_cmd+=['-Wall','-Wextra','-Werror','-mgeneral-regs-only','-I',str(ROOT/'.cache/xnu-work/osfmk')]
# The protocol cores include each other across the components that were split out on 2026-11-14
# (ShengAudioReach.c includes ShengGpr.h), and these compiles do not take touch_include, so the
# component directories go on core_cmd itself.
core_cmd+=component_include
adsp_objs=[]
for src in adsp_c:
    o=TOUCH/(src.stem+'.o');adsp_objs.append(o)
    run([str(src) if str(a)==str(coresentinel) else str(o) if str(a)==str(coreobjsentinel) else a for a in core_cmd],cwd=pr['directory'])
# The playback ladder (package/DbyAudio.cpp) is the second client of this DSP and the reason ShengApr.c is above:
# it opens the channel this board's own device tree names (kona.dtsi:4191-4216, apr_audio_svc with
# qcom,intents = <0x200 20>), walks the AFE/ADM/ASM command ladder ShengApr.c encodes, and plays one mapped PCM
# stream out of Primary TDM RX 0x9000 - the four-slot port this unit's speakers hang off (asoc/kona.c:6678-6692,
# kona/DBY/W09_VA/huawei-audio.dtsi). Its entry is sheng_audio_probe, which LegacyDisplayProbe.cpp:927 calls and
# LinkStubs.c:124 leaves a weak no-op for; until this object existed the call landed on that no-op and the four
# TFA9874 amplifiers DbySmartPA.cpp initialised never received a single sample. Same compile record as
# AdspProbe.cpp, and no late staging: the file has one zero-initialised POD at namespace scope, so it emits no
# __mod_init_func for staging to move.
audio_cpp=ROOT/'kits/dby_w09/DbyAudio.cpp';audio_obj=TOUCH/'DbyAudio.o'
run([str(audio_cpp) if str(a)==str(source) else str(audio_obj) if str(a)==str(probe) else a for a in cmd]+touch_include,
    cwd=r['directory'])
# The service iOS's audio server plug-in opens (package/ShengAudioService.cpp) is the second half of that pair: DbyAudio.cpp
# hands out the control page and the sample rings through sheng_audio_memory()/sheng_audio_request(), and the service's user
# client is what calls them back. It defines two IOKit classes, so it is staged exactly the way upstream's sheng kit stages the
# same file (upstream/sheng/scripts/build-gop-legacy-display.py:269-270 compile, :299 objcopy, archive list): the static
# initialiser leaves __mod_init_func and is globalised, because DbyAudio.cpp:434 calls it by name between preModLoad and
# postModLoad. Registering those classes from __mod_init_func as well is the duplicate OSMetaClass registration this tree has
# already panicked on.
service_cpp=ROOT/'kits/dby_w09/ShengAudioService.cpp';service_obj=TOUCH/'ShengAudioService.o'
run([str(service_cpp) if str(a)==str(source) else str(service_obj) if str(a)==str(probe) else a for a in cmd]+touch_include,
    cwd=r['directory'])
service_late=OUT/'ShengAudioService.late.o'
run([objcopy,'--remove-section=__DATA,__mod_init_func','--globalize-symbol=__GLOBAL__sub_I_ShengAudioService.cpp',service_obj,service_late])
require(code(service_obj.read_bytes(),macho(service_obj.read_bytes()))==code(service_late.read_bytes(),macho(service_late.read_bytes())),
        'Owned audio service instructions unchanged by late init staging')
archive=OUT/'libNativeLegacyDisplay.late.a';archive.unlink(missing_ok=True)
# The kernel force-loads exactly this archive, and the official sheng/qemu recipe uses that to carry the
# AppleKeyStore stand-in in it too (build-native-legacy-display.py:255): KeyStoreProbe.o plus the late-staged
# NativeKeyStoreEvents.o. Without them MKBEventsRegister returns NULL, liveactivitiesd dies at every start,
# chronod never answers SpringBoard's startup XPC, and the render server keeps the boot image on screen.
# HID and the key store stand in here because this is the force-loaded archive; touch has its own archive
# (build/dby-touch). cpufreq joins below as a real object; GPU power is still the LinkStubs no-op, because
# the A650 driver is not in this image yet.
KEY=ROOT/'build/apple-key-store-state'
print(run(['python3',ROOT/'scripts/build-arm-driver.py','NativeKeyStoreEvents'],text=True),end='')
keystore_source=ROOT/'kits/dby_w09/KeyStoreProbe.cpp'
keystore_obj=OUT/'KeyStoreProbe.o'
# native_aks_device_id answers with the owner Data volume's UUID, so the key store's device key is
# tied to the partition the keychain items live on (see package/KeyStoreProbe.cpp).
keystore_define='-DXNU_OWNER_DATA_VOLUME_UUID="'+owner_data_uuid()+'"'
run([str(keystore_source) if str(arg)==str(source) else str(keystore_obj) if str(arg)==str(probe) else arg for arg in cmd]+[keystore_define],cwd=r['directory'])
keyevents=KEY/'NativeKeyStoreEvents.o'
keyevents_late=KEY/'NativeKeyStoreEvents.late.o'
run([objcopy,'--remove-section=__DATA,__mod_init_func',
     '--globalize-symbol=__GLOBAL__sub_I_NativeKeyStoreEvents.cpp',keyevents,keyevents_late])
require(code(keyevents.read_bytes(),macho(keyevents.read_bytes()))==
        code(keyevents_late.read_bytes(),macho(keyevents_late.read_bytes())),
        'Owned event-service instructions unchanged by late init staging')
# The CPU frequency control for this board's EPSS domains (package/CpuFreqProbe.cpp, the official SM8250
# board alioth's file, taken unchanged). It exports sheng_cpufreq_probe/sheng_cpufreq_poll, which
# LegacyDisplayProbe.cpp:922 calls once and BootProgress.c:1299 calls every log round; until this object
# existed both calls landed on the weak no-op in LinkStubs.c:99, so the board never issued one vote and all
# eight cores sat on the EPSS reset level. Same compile record as the two probes above, so the flags match
# the kernel's C++ recipe, plus -mgeneral-regs-only the way alioth's recipe passes it (build-gop-legacy-
# display.py:430-459). This archive is the one the kernel link force-loads, which is the only reason an
# object no new symbol references still arrives.
cpufreq_source=ROOT/'kits/dby_w09/CpuFreqProbe.cpp'
cpufreq_obj=OUT/'CpuFreqProbe.o'
run([str(cpufreq_source) if str(arg)==str(source) else str(cpufreq_obj) if str(arg)==str(probe) else arg for arg in cmd]
    +['-mgeneral-regs-only'],cwd=r['directory'])

# Panel brightness (package/AppleARMBacklight.cpp: the sheng board's service with this board's constants; package/
# BacklightProbe.cpp: its entry; package/GeniI2c.c: the GENI engine core it drives). BacklightProbe.cpp defines
# sheng_backlight_probe, which LegacyDisplayProbe.cpp:929 calls and LinkStubs.c:134 leaves a weak no-op for: without this
# object that call lands on the no-op and the panel keeps whatever brightness the boot loader left it at.
# One engine and one LP8556 at 0x2c on qupv3_se11, 12-bit level across 0x10 and 0x11 (see the file's header for the kernel
# device tree and clock driver records each constant comes from).
bl_source=ROOT/'kits/dby_w09/AppleARMBacklight.cpp'
bl_obj=OUT/'AppleARMBacklight.o'
run([str(bl_source) if str(arg)==str(source) else str(bl_obj) if str(arg)==str(probe) else arg for arg in cmd]
    +['-Wno-missing-designated-field-initializers']+component_include,cwd=r['directory'])
bl_probe_source=ROOT/'kits/dby_w09/BacklightProbe.cpp'
bl_probe_obj=OUT/'BacklightProbe.o'
run([str(bl_probe_source) if str(arg)==str(source) else str(bl_probe_obj) if str(arg)==str(probe) else arg for arg in cmd]
    +['-Wno-missing-designated-field-initializers']+component_include,cwd=r['directory'])
# GeniI2c.c is C; the same record carries -xc++, which this source accepts unchanged (no designated initialisers, no implicit
# void* conversions, and its own header wraps every declaration in extern "C", so the symbols stay unmangled for the service).
# GeniI2c is a component repository since 2026-11-14; the tree materialises it under drivers/QcomGeniI2c/.
bl_geni_source=ROOT/'drivers/QcomGeniI2c/GeniI2c.c'
bl_geni_obj=OUT/'GeniI2c.o'
run([str(bl_geni_source) if str(arg)==str(source) else str(bl_geni_obj) if str(arg)==str(probe) else arg for arg in cmd]
    +['-Wno-missing-designated-field-initializers']+component_include,cwd=r['directory'])
# The service's static initialiser registers its classes with the kernel module; BacklightProbe.cpp calls it by name between
# preModLoad and postModLoad, so it leaves __mod_init_func and is globalised, the same staging the presenter and the key store use.
# 四颗 smartpa：两张 BOM 表 + 探测 + 主备回退。没有静态初始化，也不需要 late staging。
pa_source=ROOT/'kits/dby_w09/DbySmartPA.cpp'
pa_obj=OUT/'DbySmartPA.o'
run([str(pa_source) if str(arg)==str(source) else str(pa_obj) if str(arg)==str(probe) else arg for arg in cmd]
    +['-Wno-missing-designated-field-initializers']+component_include,cwd=r['directory'])
# 电池（package/DbyBattery.cpp 这个服务 + package/BatteryProbe.cpp 它的入口）。入口导出
# sheng_battery_probe：LegacyDisplayProbe.cpp:926 已经在调它，LinkStubs.c:118-119 今天把这个调用
# 答案成弱空桩——llvm-nm 在已建好的 kernel.macho 上，_sheng_cpufreq_probe 和 _sheng_backlight_probe
# （它们的对象已是这里的成员）是强 external，_sheng_battery_probe 还是 weak external。让这个归档多出
# 一个成员就是把那一次调用接到真实现上，机制与上面两块一模一样：归档是被 -force_load 拉进来的
# （build-xnu-4k-baseline.sh:664），不是因为有新符号引用它。
# 生产者 ABI 和发布语义抄官方同芯片板 alioth 的 ShengBattery.cpp/BatteryProbe.cpp：同一个
# native_battery_stats[16]，同一套 IOPMPowerSource 属性键。这 16 格现在有读者：package/BootLog.c:116
# 弱导入它，:1080-1083 的 [battery] 行把 16 格全打印出来，而 scripts/build-boot-progress.py:115 把
# BootLog.c 编进 boot-progress 归档（"BootLog.c 不进任何归档"是这条注释写下时的状态，后来变了）。
# 该服务自己的 kprintf 行仍然是第二个观察点——COM4 只镜像 kprintf，IOLog 上不来。换掉的只有传输：本机没有 ADSP 栈、全盘没有 adsp.mbn，
# 而这块板的 stock DTBO（b01.dts:574-620）把电量计写成 SPMI 上的 qcom,fg-gen4、充电器写成
# qcom,qpnp-smb5（b01.dts:447-470），所以直读 arbiter 的观察者引擎——那正是 DbyTouch 在本板跑通的通道。
# 只读是构造性的：本文件写出去的每个 32 位字都是带读操作码的观察者命令字；arbiter 的写通道窗口 chnls
# （0x0c600000 + 0x2000000）在这里和 DbyTouch 里都从不映射。
battery_source=ROOT/'kits/dby_w09/DbyBattery.cpp'
battery_obj=OUT/'DbyBattery.o'
run([str(battery_source) if str(arg)==str(source) else str(battery_obj) if str(arg)==str(probe) else arg for arg in cmd]
    +['-Wno-missing-designated-field-initializers'],cwd=r['directory'])
battery_probe_source=ROOT/'kits/dby_w09/BatteryProbe.cpp'
battery_probe_obj=OUT/'BatteryProbe.o'
run([str(battery_probe_source) if str(arg)==str(source) else str(battery_probe_obj) if str(arg)==str(probe) else arg for arg in cmd]
    +['-Wno-missing-designated-field-initializers'],cwd=r['directory'])
# DbyBattery.cpp 的静态初始化把类注册进内核模块，所以它要和 AppleARMBacklight、NativeKeyStoreEvents、
# NativeGOPPresenter 一样：初始化函数从 __mod_init_func 里摘出去、改名成全局符号，由 BatteryProbe.cpp 在
# preModLoad 与 postModLoad 之间按名字调用。留着不改会同一个类注册两次，本树已经为重复注册在
# IOBlockStorageDevice 上 panic 过一次。符号名不是猜的：llvm-nm 在这个对象上是
# (__TEXT,__text) non-external __GLOBAL__sub_I_DbyBattery.cpp。
battery_late=OUT/'DbyBattery.late.o'
run([objcopy,'--remove-section=__DATA,__mod_init_func','--globalize-symbol=__GLOBAL__sub_I_DbyBattery.cpp',battery_obj,battery_late])
require(code(battery_obj.read_bytes(),macho(battery_obj.read_bytes()))==code(battery_late.read_bytes(),macho(battery_late.read_bytes())),
        'Owned battery instructions unchanged by late init staging')
bl_late=OUT/'AppleARMBacklight.late.o'
run([objcopy,'--remove-section=__DATA,__mod_init_func','--globalize-symbol=__GLOBAL__sub_I_AppleARMBacklight.cpp',bl_obj,bl_late])
require(code(bl_obj.read_bytes(),macho(bl_obj.read_bytes()))==code(bl_late.read_bytes(),macho(bl_late.read_bytes())),
        'Owned backlight instructions unchanged by late init staging')
run(['xcrun','ar','rcs',archive,late,OUT/'original-bindings.o',probe,keystore_obj,keyevents_late,hid_cpp_obj,hid_drv_obj,hid_bindings_obj,cpufreq_obj,bl_late,bl_probe_obj,bl_geni_obj,pa_obj,battery_late,battery_probe_obj,adsp_obj,*adsp_objs,audio_obj,service_late])
sources=[ROOT/'drivers/GopFramebuffer/NativeLegacyDisplay.cpp',
         ROOT/'drivers/GopFramebuffer/NativeLegacyRequest.h',source,
         keystore_source,ROOT/'drivers/AppleKeyStoreState/NativeKeyStoreEvents.cpp',hid_cpp,
         cpufreq_source,ROOT/'kits/dby_w09/SmmuBypass.h',bl_source,bl_probe_source,bl_geni_source,pa_source,battery_source,battery_probe_source,adsp_cpp,audio_cpp,service_cpp,*adsp_c]
# The presenter is its own official component build; only its initializer registration moves to late staging.
print(run(['python3',ROOT/'scripts/build-arm-driver.py','NativeGOPPresenter'],text=True),end='')
gop=ROOT/'build/native-gop-presenter';gop_late=gop/'NativeGOPPresenter.late.o'
run([objcopy,'--remove-section=__DATA,__mod_init_func','--globalize-symbol=__GLOBAL__sub_I_NativeGOPPresenter.cpp',
     gop/'NativeGOPPresenter.o',gop_late])
require(code((gop/'NativeGOPPresenter.o').read_bytes(),macho((gop/'NativeGOPPresenter.o').read_bytes()))==
        code(gop_late.read_bytes(),macho(gop_late.read_bytes())),'Owned presenter instructions unchanged by late init staging')
run(['xcrun','ar','rcs',archive,gop_late])
sources.extend([ROOT/'drivers/GopFramebuffer/NativeGOPPresenter.cpp',ROOT/'drivers/GopFramebuffer/NativeGOPWire.h'])
(OUT/'build-report.json').write_text(json.dumps(dict(
    source_sha256={str(p.relative_to(ROOT)):sha(p) for p in sources},generated_abi_sha256=sha(OUT/'OriginalLegacyFramebufferABI.h'),
    original_candidate_sha256=sha(candidate),original_audit_sha256=sha(ROOT/'build/display-analysis/mobile-graphics-legacy-abi.json'),
    compiled_object_sha256=sha(obj),late_object_sha256=sha(late),archive_sha256=sha(archive),
    bindings={n:hex(v) for n,v in bindings.items()},virtual_entries=358,owned_entries=owned,inherited_native_entries=native,
    guarded_backend_leaves=[i for i in pure if i not in (213,337)],original_instructions_modified=False,driver_initialized=False,display_presented=False,desktop=False),indent=2)+'\n')
print('Built genuine original Legacy inheritance/dispatch; seven backend leaves still guarded, runtime lifecycle unqualified')

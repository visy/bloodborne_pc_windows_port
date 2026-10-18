"""Compile selected shadPS4/GoldHEN XML patches into out/patches.bin for the loader.

Patch addresses are PS4 virtual addresses (eboot base 0x400000); the loader's
image places eboot vaddr 0 at image offset 0. Only literal writes are supported
(bytes, bytes16/32/64, float32/64, utf8, utf16); pattern ("mask") patches are rejected.
"""
import argparse
import hashlib
import json
import os
import struct
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

_SCRIPTS=str(Path(__file__).resolve().parent)
if _SCRIPTS not in sys.path:
    sys.path.insert(0,_SCRIPTS)
from party_env import party_enabled, party_switch_on, party_value_on  # noqa: E402  (the shared BB_PARTY rule)

EBOOT_BASE=0x400000
# BB_FPS presets: patch names from patches/Bloodborne.xml (app version 01.09).
# Above 60 FPS the sprint fix always goes with the frame rate patch: without it sprinting drops to
# half speed (the game's wall detector measured distance per frame, a 30 FPS rule).
FPS_PRESETS={'30':[],'60':['60 FPS++'],'90':['90 FPS++','Sprint Fix (High FPS)'],
             'uncap':['Uncap FPS++','Sprint Fix (High FPS)']}
# Above 30 FPS the frame rate is done by hooks in the game's code (gpu/shim/bbport_frame_rate.cpp,
# deadinside's 0.6-pre1): no FPS++ patches. BB_FRAME_HOOKS=0 keeps the patches (the fallback and
# comparisons). The default must match HooksDefault in gpu/shim/bbport_frame_rate_logic.h.
FRAME_HOOKS_DEFAULT=True


def frame_hooks(fps, env=os.environ):
    """Does the GPU library run this frame rate by hooks (bbport_frame_rate_logic.h HooksWanted)?"""
    off=lambda v: v in ('0','off','false')
    if not fps or fps=='30' or off(env.get('BB_FPS_PATCH')):
        return False
    value=env.get('BB_FRAME_HOOKS','')
    return not off(value) if value else FRAME_HOOKS_DEFAULT


def preset_patches(fps, env=os.environ):
    """The FPS++ patches for BB_FPS: none with the hooks or with the FPS patch off."""
    if off_patch(env) or frame_hooks(fps, env):
        return []
    return FPS_PRESETS[fps]


# The bytes the frame rate hooks rewrite or read instead of the game's (image offsets, length):
# gpu/shim/bbport_frame_rate.cpp's detours, its redirected instructions (Redirects) and the
# frame-tied constants they read (tests/test_patches.py checks the list against the C++ source).
# With the hooks on, a patch that writes any of them (an FPS patch of its own, e.g. "60FPS (no
# deltatime)", or another patch of the same code) is left out with a message: on top of the hooks
# the two would each rescale the same steps. BB_FRAME_HOOKS=0 applies it with the FPS++ patches.
FRAME_HOOK_DETOURS=[(0x203493d,8),(0x2034d96,9),(0x2018e36,7),(0x17f9b71,7),(0x1572a7d,7),(0x1514c24,8)]
FRAME_HOOK_REDIRECTS=[
    0xab9f0d,0xdca3ed,0xee596a,0xee5a8a,0xee5baa,0xef06f1,0xef6992,0x1044943,0x12a4416,0x15dc1f7,
    0x173456e,0x17f9d26,0x180f647,0x18bc4f7,0x18bc50d,0x1dbc308,0x227784b,0x2277892,0x2313d8d,0x2313dad,
    0x2313db7,0x2315d71,0x1515fab,0x1516030,0x19084f5,0x1908522,0x1908518,0x190854c,0x143e2b2,0x143af8c,
    0x1a4abca,0x1514c4d,0x47c6dc,0x7e45be,0x7e4642,0x7e6239,0x7f706d,0x7f70ad,0x7f79e5,0x7f7be8,
    0x7f7dbd,0x805898,0x805d18,0x806269,0x8062f0,0x809d62,0x80a4c9,0x80a876]
FRAME_HOOK_CONSTANTS=[
    0x47693f0,0x47b63e4,0x47b8474,0x47b91c0,0x47b98b8,0x47efdcc,0x4888688,0x48ad65c,0x48ad660,0x48ad664,
    0x48adc08,0x48add28,0x48cc3f4,0x490b5b0,0x492577c,0x4925e68,0x4925e94,0x4926334,0x4926364,0x4926c9c,
    0x49270e4,0x4927554,0x49275f4,0x4927c4c,0x4927fd4,0x4927fe0,0x4928e98,0x4929b80,0x4929e70,0x4929e74]
FRAME_HOOK_SITES=(FRAME_HOOK_DETOURS+[(a,8) for a in FRAME_HOOK_REDIRECTS]
                  +[(a,16 if a==0x490b5b0 else 4) for a in FRAME_HOOK_CONSTANTS])


def touches_frame_hooks(writes):
    """True if any (offset, bytes) write overlaps FRAME_HOOK_SITES (overlaps(), as for the presets)."""
    return overlaps([(site,bytes(n)) for site,n in FRAME_HOOK_SITES],writes)


def metadata_writes(meta):
    """A patch's (offset, bytes) writes, without the eboot check; [] when a line cannot be read."""
    try:
        return [(int(line.get('Address') or '',0)-EBOOT_BASE,encode(line)) for line in meta.iter('Line')]
    except ValueError:
        return []


def without_frame_hook_conflicts(items, writes_of, label):
    """items whose writes (writes_of(item)) leave the hooks' bytes alone; the others are reported."""
    kept=[]
    for item in items:
        if touches_frame_hooks(writes_of(item)):
            print(f'Patches: {label(item)} left out: it changes code or constants the frame rate hooks '
                  f'rewrite (BB_FRAME_HOOKS=0 applies it, with the FPS++ patches)',file=sys.stderr)
        else:
            kept.append(item)
    return kept


def off_patch(env):
    return env.get('BB_FPS_PATCH') in ('0','off','false')
# Upscaler presets (bbport.ini "preset", the in-game menu): output / render size ratio. The game
# then renders at 1920x1080 / ratio and the port's temporal upscaler restores the output size.
OUTPUT_SIZE=(1920,1080)
PRESET_SCALES=[1.0,1.5,1.7,2.0,3.0]
# The community patch changes two independent consumers: the game render/window setup
# and the UI movie viewport. Keep the latter at native size so glyph rasterisation and
# vector tessellation do not inherit the scene's FSR resolution.
RESOLUTION_TEMPLATE='Resolution Patch 1280x720 (16:9)'
# Outputs wider than 16:9 (from Supermedo/bloodborne_pc PR #70 by emurphy04): Kyo's 21:9 patch
# widens the camera's aspect ratio, the HUD canvas (1080 high, wider), the HUD projection and the
# screen-to-HUD mapping of the lock-on and HP-bar markers; ultrawide_writes recomputes those
# values for the output's own aspect ratio (byte-identical to Kyo's at 2560x1080).
ULTRAWIDE_TEMPLATE='Resolution Patch 2560x1080 (21:9)'
ULTRAWIDE_CANVAS=2560 # the HUD canvas width written by ULTRAWIDE_TEMPLATE
CAMERA_ASPECT=0x0183A35D-EBOOT_BASE
HUD_PROJECTION_WIDTH=0x04D26EA4-EBOOT_BASE
MARKER_BIAS=(0x04CF9A10-EBOOT_BASE,0x04CF9A70-EBOOT_BASE,0x04CF9AE0-EBOOT_BASE)
# Effect switches (bbport.ini, in-game menu, launcher): key -> (patch when the key is 0, patch
# when it is 1). The game reads these at start; a change applies after a restart.
EFFECTS={
    'effect_chromatic_aberration':('Disable Chromatic Aberration',None),
    'effect_dof':('Disable DoF',None),
    'effect_motion_blur':('Disable Motion Blur (perf increase)',None),
    'effect_ssao':('Disable SSAO',None),
    'effect_game_aa':('Disable AA',None),
    'effect_dynamic_shadows':('Disable Dynamic Light Shadows (perf increase)',None),
    'effect_ssr':(None,'Enable Screen Space Reflections (READ NOTE)'),
    'skip_intro':(None,'Skip Intro'),
    'debug_camera':(None,'Restore Debug Camera'),
    'debug_menu':(None,'Restore Debug Menu (READ NOTES)'),
}
# Intel CPUs: the game's tone mapping turns black (DLC areas most; reported as darker than on the
# PS4): the game code runs natively, and Intel's approximate float instructions differ from the
# PS4's AMD CPU, so a block the game gates on a computed flag never runs. The community fix runs
# it always; on by default on Intel. BB_INTEL_TONEMAP_FIX=0/1 forces it off or on.
INTEL_TONEMAP='Intel Black Tonemap Fix'


def intel_cpu(cpuinfo=None, env=os.environ):
    """An Intel CPU: from `cpuinfo` (/proc/cpuinfo format) when given, else from Windows'
    PROCESSOR_IDENTIFIER or Linux's /proc/cpuinfo."""
    if cpuinfo is None and os.name == 'nt':
        proc_id = env.get('PROCESSOR_IDENTIFIER', '')
        if proc_id:  # 'Intel64 Family 6 Model 151 Stepping 2, GenuineIntel'
            return 'Intel' in proc_id
        try:
            import platform
            return 'Intel' in platform.processor()
        except Exception:
            return False
    try:
        with open(cpuinfo or '/proc/cpuinfo') as f:
            return any(line.startswith('vendor_id') and 'GenuineIntel' in line for line in f)
    except OSError:
        return False


# The title's PLAY ONLINE / PLAY OFFLINE dialog: the port has no PSN, the game goes straight to the
# main menu offline. On by default (the launcher's switch); BB_SKIP_NETWORK_CHOICE=0 shows it,
# BB_SKIP_NETWORK_CHOICE=online goes online instead (party mode; the launcher sets it).
SKIP_NETWORK_CHOICE='Skip Online/Offline Choice'


SKIP_NETWORK_CHOICE_ONLINE='Party: Skip Online/Offline Choice (Online)'


def skip_network_choice(env=os.environ):
    return env.get('BB_SKIP_NETWORK_CHOICE','1')!='0'


def network_choice_patch(env=os.environ, available=None):
    """The title-dialog patch for BB_SKIP_NETWORK_CHOICE: '0' none (the game asks), 'online' the
    PLAY ONLINE variant (party mode; none, with a warning, when the XML lacks it), else PLAY OFFLINE."""
    # Party mode (BB_PARTY set) boots ONLINE through the party NP layer unless told otherwise.
    value=env.get('BB_SKIP_NETWORK_CHOICE','online' if party_mode(env) else '1').strip().lower()
    if value=='0':
        return None
    if value=='online':
        if available is not None and SKIP_NETWORK_CHOICE_ONLINE not in available:
            print(f'Patches: BB_SKIP_NETWORK_CHOICE=online but "{SKIP_NETWORK_CHOICE_ONLINE}" is not in the '
                  'patch file; the title asks PLAY ONLINE / PLAY OFFLINE',file=sys.stderr)
            return None
        return SKIP_NETWORK_CHOICE_ONLINE
    return SKIP_NETWORK_CHOICE


# Party co-op (BB_PARTY set): the seamless patches (docs/party/seamless_rules.md), unless
# BB_PARTY_SEAMLESS=0. The community "Disable HTTP Requests" patch is never applied in party mode
# (the party's FROM API runs over the game's HTTP calls).
PARTY_SEAMLESS=['Party: Bells anywhere','Party: Bells after boss defeated',
                'Party: Keep session on map reload','Party: SOS sign timeout 30s']
PARTY_NO_INSIGHT='Party: Bells without Insight'
# Join timing (docs/party/join_timing.md): the multiplayer banners' hold and the bell "Searching..."
# banners (BB_PARTY_FAST_MESSAGES=0 turns them off); the game's own join / summon pacing
# (BB_PARTY_FAST_JOIN=0 turns it off). Both on by default in party mode, independent of
# BB_PARTY_SEAMLESS, and left out of the party version check (Party="cosmetic").
PARTY_FAST_MESSAGES=['Party: Fast multiplayer messages','Party: Hide bell searching messages']
PARTY_FAST_JOIN=['Party: Fast join','Party: Fast rejoin']
# With fast join unless BB_PARTY_FAST_SIGN=0: the first sign 0.5 s after a bell (docs/party/join_timing.md).
PARTY_FAST_SIGN='Party: Fast bell sign'
DISABLE_HTTP='Disable HTTP Requests'


def party_mode(env=os.environ):
    """BB_PARTY turns party mode on (party_env.py: unset, empty, 0, off, false, no = off)."""
    return party_enabled(env)


def party_patches(env=os.environ):
    if not party_mode(env):
        return []
    names=[]
    if env.get('BB_PARTY_SEAMLESS','1').strip()!='0':
        names+=PARTY_SEAMLESS
        if env.get('BB_PARTY_BELL_NO_INSIGHT','1').strip()!='0':
            names.append(PARTY_NO_INSIGHT)
    if party_switch_on(env,'BB_PARTY_FAST_JOIN'):
        names+=PARTY_FAST_JOIN
    if party_switch_on(env,'BB_PARTY_FAST_JOIN') and party_switch_on(env,'BB_PARTY_FAST_SIGN'):
        names.append(PARTY_FAST_SIGN)
    if party_switch_on(env,'BB_PARTY_FAST_MESSAGES'):
        names+=PARTY_FAST_MESSAGES
    return names


# Party version check (gpu/shim/party/party_runtime.cpp, PartyLink HELLO): the host admits a guest
# only with the same eboot.bin, the same gameplay patches and the same gameplay mods. Patches that
# change only what one machine draws or plays back (graphics, frame rate, resolution, LOD, sound
# workarounds, camera and input feel, language) are left out of that check, so friends can play
# with different graphics settings. Every other patch counts: the party patches, the skip-online
# dialog, rally / physics / debug / cheat patches and any patch not listed here (external ones
# too, unless their XML marks them Party="cosmetic").
COSMETIC_PATCHES={
    # Frame rate (the 30 FPS rule's speed fixes go with them).
    '30 FPS++','60 FPS++','90 FPS++','Uncap FPS++','Sprint Fix (High FPS)',
    # Resolution / render size / memory for it.
    'Optimal 1080p','Increased Graphics Heap Sizes',
    # Effects and level of detail.
    'Disable AA','Disable Chromatic Aberration','Disable DoF','Disable Dynamic Light Shadows (perf increase)',
    'Disable Motion Blur (perf increase)','Disable SSAO','Enable Screen Space Reflections (READ NOTE)',
    'Model LOD -2 (Highest)','Model LOD 1 (Lower)','Model LOD 2 (Lowest)','Performance Patch (perf increase)',
    # Platform workarounds (CPU / sound).
    'Intel Black Tonemap Fix','Intel 12th Gen+ SFX workaround','FMOD Crash Fix',
    # Presentation, camera and input on this machine only.
    '50% Text scale','Skip Intro','Unlock Game Region','Bookmark and Capture outputs',
    'Increased camera distance','Disable Camera Auto Rotation via Movement','Sensitive Analog Input (easier to run)',
    # Party join timing and banners (BB_PARTY_FAST_JOIN / BB_PARTY_FAST_MESSAGES): local pacing only.
    'Party: Fast join','Party: Fast rejoin','Party: Fast bell sign','Party: Fast multiplayer messages','Party: Hide bell searching messages',
}
COSMETIC_PREFIXES=('Resolution Patch ',)
COSMETIC_MARKS=('Light Grid',)
PARTY_PATCH_FILE='party_patch_hash.txt'


def patch_is_cosmetic(name, meta=None):
    """True when patch `name` changes no gameplay (left out of the party version check)."""
    if meta is not None and meta.get('Party','').strip().lower() in ('cosmetic','gameplay'):
        return meta.get('Party').strip().lower()=='cosmetic'
    return (name in COSMETIC_PATCHES or name.startswith(COSMETIC_PREFIXES)
            or any(mark in name for mark in COSMETIC_MARKS))


def party_patch_set(items):
    """(hash hex, names) of the gameplay patches. items: [(name, meta or None, writes)]; cosmetic
    ones are dropped; the hash covers names and bytes, independent of order."""
    gameplay=sorted((name,writes) for name,meta,writes in items if not patch_is_cosmetic(name,meta))
    digest=hashlib.sha256(b'bbparty-patches-1')
    for name,writes in gameplay:
        digest.update(b'\0patch\0'+name.encode('utf-8')+b'\0')
        for offset,data in writes:
            digest.update(struct.pack('<QQ',offset,len(data))+data)
    return digest.hexdigest(),[name for name,_ in gameplay]


def write_party_patch_file(out, digest, names, cosmetic=()):
    lines=['# Party version check (scripts/patches.py): the gameplay patches of patches.bin. Players',
           '# with another hash are refused by the party host; cosmetic patches do not count.',
           f'hash {digest}']
    lines+=[f'patch {n}' for n in names]
    lines+=[f'# cosmetic {n}' for n in cosmetic]
    (Path(out)/PARTY_PATCH_FILE).write_text('\n'.join(lines)+'\n',encoding='utf-8')


def intel_tonemap_fix(env=os.environ, cpuinfo=None):
    forced=env.get('BB_INTEL_TONEMAP_FIX')
    return forced=='1' if forced in ('0','1') else intel_cpu(cpuinfo, env)

# model_lod: -2 highest, 0 the game's, 1 lower, 2 lowest.
MODEL_LOD={'-2':'Model LOD -2 (Highest)','1':'Model LOD 1 (Lower)','2':'Model LOD 2 (Lowest)'}


DEBUG_MENU='Restore Debug Menu (READ NOTES)'
# What the patched game reads for the debug menu (BB_FILE_TRACE=1), under dvdroot_ps4. The game asks
# for lower-case names; the PS4's file system and the port's runtime ignore case.
DEBUG_MENU_FONTS=('adhoc/font/DbgFont14h.ccm','adhoc/font/DbgFont14h.tpf')
DEBUG_MENU_SHADERS=('adhoc/FontShader/debugFont_vs.vpo','adhoc/FontShader/debugFont_ps.ppo')


def find_ignoring_case(root, relative):
    """`root`/`relative`, each component matched ignoring case; None when absent."""
    path=Path(root)
    for part in relative.split('/'):
        exact=path/part
        if exact.exists():
            path=exact
            continue
        try:
            matches=[e for e in path.iterdir() if e.name.casefold()==part.casefold()]
        except OSError:
            return None
        if not matches:
            return None
        path=matches[0]
    return path


def debug_menu_problem(game):
    """Why the debug menu patch cannot work with this game folder, or None."""
    dvdroot=Path(game)/'dvdroot_ps4'
    missing=[]
    for relative in DEBUG_MENU_FONTS:
        found=find_ignoring_case(dvdroot,relative)
        if not found or not found.is_file() or found.stat().st_size==0:
            missing.append(relative)
    if not missing:
        return None
    hint=''
    if all(find_ignoring_case(dvdroot,'font/'+r.rsplit('/',1)[1]) for r in missing):
        hint=' (they are in dvdroot_ps4/font: the game reads them from dvdroot_ps4/adhoc/font)'
    return ('the debug menu needs the font files from https://www.nexusmods.com/bloodborne/mods/253 '
            f'in dvdroot_ps4 of the game folder or of a mod: missing {", ".join(missing)}{hint}')


def validate_patch_requirements(names, game):
    """The patches to apply: raises for conflicts; drops the debug menu (with a message) when its
    font files are missing, as the game crashes opening the menu without them."""
    if 'Restore Debug Camera' in names and 'Enemy Control' in names:
        raise ValueError('Restore Debug Camera conflicts with Enemy Control; enable only one')
    if DEBUG_MENU in names:
        problem=debug_menu_problem(game)
        if problem:
            print(f'Patches: debug menu off: {problem}',file=sys.stderr)
            return [n for n in names if n!=DEBUG_MENU]
        absent=[r for r in DEBUG_MENU_SHADERS if not find_ignoring_case(Path(game)/'dvdroot_ps4',r)]
        if absent:
            print(f'Patches: debug menu: {", ".join(absent)} not found (the mod\'s font shaders); '
                  'the menu may not draw',file=sys.stderr)
    return names


def effect_patches(settings):
    names=[]
    for key,(off,on) in EFFECTS.items():
        if key not in settings: continue
        name=on if settings[key]=='1' else off
        if name: names.append(name)
    lod=MODEL_LOD.get(settings.get('model_lod','0'))
    if lod: names.append(lod)
    return names
SCENE_WIDTH=0x02196A6B-EBOOT_BASE
SCENE_HEIGHT=0x02196A7A-EBOOT_BASE
UI_WIDTH=0x02358554-EBOOT_BASE
UI_HEIGHT=0x0235855D-EBOOT_BASE


def read_settings(path):
    settings={}
    if path.exists():
        for line in path.read_text(encoding='utf-8').splitlines():
            key,sep,value=line.partition('=')
            if sep and not line.startswith('#'): settings[key.strip()]=value.strip()
    return settings


def render_size(settings,override=''):
    """Render resolution for the upscaler preset, or None for native."""
    if override:
        w,h=(int(v) for v in override.lower().split('x'))
        return (w,h)
    if settings.get('upscaler','fsr3')=='off': return None
    preset=int(settings.get('preset','0') or 0)
    scale=PRESET_SCALES[max(0,min(preset,len(PRESET_SCALES)-1))]
    if scale==1.0: return None
    # Even sizes (the game has half-resolution buffers).
    return tuple(max(2,round(v/scale/2)*2) for v in OUTPUT_SIZE)


def output_size(settings):
    """Output (UI) size from bbport.ini output_res, e.g. 3840x2160; 1920x1080 by default."""
    try:
        w,h=(int(v) for v in settings.get('output_res','').lower().split('x'))
        if w>0 and h>0: return (w,h)
    except ValueError:
        pass
    return OUTPUT_SIZE


def scaled_sizes(settings):
    """(render, output) for an output other than 1080p (above it, or 720p for the Steam Deck):
    the game renders at output / preset scale (or at the output size without upscaler) and the
    upscaler fills the output. None at 1080p and for TAA (native, live host targets only)."""
    out=output_size(settings)
    if out==OUTPUT_SIZE or settings.get('upscaler')=='taa': return None
    scale=1.0
    if settings.get('upscaler','fsr3')!='off':
        preset=int(settings.get('preset','0') or 0)
        scale=PRESET_SCALES[max(0,min(preset,len(PRESET_SCALES)-1))]
    render=tuple(max(2,round(v/scale/2)*2) for v in out)
    # A scene of exactly the UI coordinate space's size (1920x1080 at 4K Performance, the
    # ultrawide HUD canvas at 2560x1080 or 3840x1080 native) is indistinguishable from it: the
    # port's UI composition recognizes UI draws by that size.
    canvas=hud_canvas(out)
    if render==canvas: render=(canvas[0]-4,canvas[1]-2)
    return render,out


def hud_canvas(output):
    """The HUD's coordinate space for an output: 1920x1080 up to 16:9, wider (still 1080 high,
    even width) for ultrawide outputs, e.g. 2580x1080 for 3440x1440. The port scales it to the
    output uniformly (UiComposition::Canvas, BB_UI_RES from run.bat)."""
    w,h=output
    if w*OUTPUT_SIZE[1]<=h*OUTPUT_SIZE[0]: return OUTPUT_SIZE
    return (round(OUTPUT_SIZE[1]*w/h/2)*2,OUTPUT_SIZE[1])


def ultrawide_writes(xml,output,app_version,segments):
    """ULTRAWIDE_TEMPLATE for `output`'s aspect ratio, without its scene and UI viewport sizes
    (resolution_writes). After resolution_writes: its 1920-wide HUD writes are replaced."""
    canvas=hud_canvas(output)[0]
    viewport={SCENE_WIDTH,SCENE_HEIGHT,UI_WIDTH,UI_HEIGHT}
    floats={CAMERA_ASPECT:(ULTRAWIDE_CANVAS/1080,output[0]/output[1]),
            HUD_PROJECTION_WIDTH:(ULTRAWIDE_CANVAS,canvas)}
    floats.update({site:(0.5*1920/ULTRAWIDE_CANVAS,0.5*1920/canvas) for site in MARKER_BIAS})
    old_width=ULTRAWIDE_CANVAS.to_bytes(3,'little')
    out=[]
    found=set()
    for offset,data in compile_patches(xml,[ULTRAWIDE_TEMPLATE],app_version,segments):
        if offset in viewport: continue
        if offset in floats:
            old,value=floats[offset]
            if len(data)!=4 or abs(struct.unpack('<f',data)[0]-old)>1e-3:
                raise ValueError(f'unexpected ultrawide value at {offset+EBOOT_BASE:#x}')
            data=struct.pack('<f',value)
            found.add(offset)
        elif len(data)==4 and data[0] in (0xB8,0xB9,0xBE) and data[1:]==old_width:
            # mov e?x, 2560 (the imm32's last byte is the next write's): the HUD canvas width.
            data=bytes([data[0]])+canvas.to_bytes(3,'little')
        out.append((offset,data))
    if found!=floats.keys():
        raise ValueError('ultrawide patch is missing camera/HUD values')
    return out


def resolution_writes(xml,size,app_version,segments,ui=OUTPUT_SIZE):
    writes=compile_patches(xml,[RESOLUTION_TEMPLATE],app_version,segments)
    replacements={SCENE_WIDTH:(0xB8,0x500,size[0]),
                  SCENE_HEIGHT:(0xB8,0x2D0,size[1]),
                  UI_WIDTH:(0xB8,0x500,ui[0]),
                  UI_HEIGHT:(0xB9,0x2D0,ui[1])}
    out=[]
    seen=set()
    for offset,data in writes:
        if offset in replacements:
            opcode,old,value=replacements[offset]
            if data!=bytes([opcode])+old.to_bytes(3,'little') or offset in seen:
                raise ValueError(f'unexpected resolution patch at {offset+EBOOT_BASE:#x}')
            data=bytes([opcode])+value.to_bytes(3,'little')
            seen.add(offset)
        out.append((offset,data))
    if seen!=replacements.keys():
        raise ValueError('resolution patch is missing scene/UI viewport instructions')
    return out


def eboot_segments(elf):
    phoff,=struct.unpack_from('<Q',elf,0x20)
    phentsize,phnum=struct.unpack_from('<HH',elf,0x36)
    segments=[]
    for i in range(phnum):
        kind,_,_,vaddr,_,_,memsz,_=struct.unpack_from('<IIQQQQQQ',elf,phoff+i*phentsize)
        if kind==1: segments.append((vaddr,vaddr+memsz))
    return segments


def elf_bytes(elf, vaddr, size):
    """The eboot's file bytes at virtual address `vaddr` (patch offset), or None."""
    phoff,=struct.unpack_from('<Q',elf,0x20)
    phentsize,phnum=struct.unpack_from('<HH',elf,0x36)
    for i in range(phnum):
        kind,_,offset,start,_,filesz,_,_=struct.unpack_from('<IIQQQQQQ',elf,phoff+i*phentsize)
        if kind==1 and start<=vaddr and vaddr+size<=start+filesz:
            return elf[offset+vaddr-start:offset+vaddr-start+size]
    return None


def originals_match(xml, name, app_version, elf):
    """False (with a message) when a line of patch `name` carries Original="hex" (the 1.09 bytes
    the patch replaces) and the eboot holds something else there: the patch is then left out."""
    for meta in ET.parse(xml).getroot().iter('Metadata'):
        if meta.get('Name')!=name or meta.get('AppVer')!=app_version: continue
        for line in meta.iter('Line'):
            original=line.get('Original')
            if not original: continue
            want=bytes.fromhex(original.replace(' ',''))
            have=elf_bytes(elf,int(line.get('Address'),0)-EBOOT_BASE,len(want))
            if len(want)!=len(encode(line)) or have!=want:
                print(f'Patches: "{name}" left out: {line.get("Address")} holds '
                      f'{have.hex() if have else "nothing"}, not {want.hex()} (another eboot?)',file=sys.stderr)
                return False
    return True


def encode(line):
    kind,value=line.get('Type'),line.get('Value')
    if kind=='bytes': return bytes.fromhex(value.replace(' ',''))
    if kind in ('bytes16','bytes32','bytes64'):
        return int(value,0).to_bytes(int(kind[5:])//8,'little')
    if kind=='float32': return struct.pack('<f',float(value))
    if kind=='float64': return struct.pack('<d',float(value))
    if kind=='utf8': return value.encode()+b'\0'
    if kind=='utf16': return value.encode('utf-16-le')+b'\0\0'
    raise ValueError(f'unsupported patch type {kind!r}')


def compile_patches(xml, names, app_version, segments):
    found={}
    for meta in ET.parse(xml).getroot().iter('Metadata'):
        if meta.get('Name') in names and meta.get('AppVer')==app_version and meta.get('AppElf','eboot.bin')=='eboot.bin':
            found[meta.get('Name')]=meta
    missing=[n for n in names if n not in found]
    if missing: raise ValueError(f'patches not found for app version {app_version}: {missing}')
    writes=[]
    for name in names:
        for line in found[name].iter('Line'):
            offset=int(line.get('Address'),0)-EBOOT_BASE
            data=encode(line)
            if not any(start<=offset and offset+len(data)<=end for start,end in segments):
                print(f'Warning: {name}: address {line.get("Address")} is outside the eboot; skipping', file=sys.stderr)
                continue
            writes.append((offset,data))
    return writes


# Third-party patch files (shadPS4/GoldHEN XML) in the data directory's patches/ folder.
# The retail releases (scripts/game_check.py SUPPORTED_TITLES): one 1.09 executable.
BLOODBORNE_IDS={'CUSA00900','CUSA00207','CUSA00208','CUSA00299','CUSA01363',
                'CUSA03179','CUSA03173','CUSA03014','CUSA03023'}


def external_patches(directory, app_version='01.09', exclude=Path(__file__).resolve().parent.parent/'patches/Bloodborne.xml'):
    """[(key, file, metadata)] of eboot patches for this version in directory/*.xml.
    key is "<file name>/<patch name>" (the launcher's selection, patches.json)."""
    found=[]
    directory=Path(directory)
    for path in sorted(directory.glob('*.xml')) if directory.is_dir() else []:
        if path.resolve()==Path(exclude).resolve(): continue  # the built-in file (patches/ in a checkout)
        try:
            root=ET.parse(path).getroot()
        except ET.ParseError as error:
            print(f'Patches: {path.name}: {error}',file=sys.stderr)
            continue
        ids={e.text.strip() for e in root.iter('ID') if e.text}
        if ids and not ids&BLOODBORNE_IDS: continue
        for meta in root.iter('Metadata'):
            if meta.get('AppVer')==app_version and meta.get('AppElf','eboot.bin')=='eboot.bin':
                found.append((f'{path.name}/{meta.get("Name")}',path,meta))
    return found


def external_selection(found, config):
    """Selected external patches: patches.json {"enabled": [...], "disabled": [...]} overrides
    each file's isEnabled."""
    settings={}
    if config and Path(config).is_file():
        settings=json.loads(Path(config).read_text(encoding='utf-8'))
    enabled,disabled=set(settings.get('enabled',[])),set(settings.get('disabled',[]))
    return [(key,path,meta) for key,path,meta in found
            if key in enabled or (key not in disabled and meta.get('isEnabled','false').lower()=='true')]


def overlaps(writes_a, writes_b):
    """True if any write of writes_a touches a byte of writes_b."""
    ranges=[(offset,offset+len(data)) for offset,data in writes_a]
    return any(start<offset+len(data) and offset<end
               for offset,data in writes_b for start,end in ranges)


def compile_external(selected, segments, applied=None):
    """Writes of the selected external patches; a patch with unsupported lines is skipped whole.
    applied: a list that gets (key, metadata, writes) of each applied patch."""
    writes=[]
    for key,_,meta in selected:
        try:
            ours=[]
            for line in meta.iter('Line'):
                offset=int(line.get('Address') or '',0)-EBOOT_BASE
                data=encode(line)
                if not any(start<=offset and offset+len(data)<=end for start,end in segments):
                    raise ValueError(f'address {line.get("Address")} is outside the eboot')
                ours.append((offset,data))
        except ValueError as error:
            print(f'Patches: skipped {key}: {error}',file=sys.stderr)
            continue
        writes+=ours
        if applied is not None: applied.append((key,meta,ours))
        print(f'Patches: external {key} ({len(ours)} writes)')
    return writes


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--patches-dir',type=Path,help='third-party patch XML files (shadPS4 format)')
    p.add_argument('--patches-config',type=Path,help='patches.json: enabled/disabled external patches')
    p.add_argument('--xml',type=Path,default=Path(__file__).resolve().parent.parent/'patches/Bloodborne.xml')
    p.add_argument('--fps',choices=sorted(FPS_PRESETS),default='uncap')
    p.add_argument('--extra',default='',help='additional patch names, separated by ";"')
    p.add_argument('--app-version',default='01.09')
    p.add_argument('--out',type=Path,default=Path(__file__).resolve().parent.parent/'out')
    p.add_argument('--settings',type=Path,default=Path(__file__).resolve().parent.parent/'bbport.ini')
    p.add_argument('--game-dir',type=Path,default=Path(os.environ.get('BB_GAME_DIR','../CUSA03173')))
    p.add_argument('--render-res',default='',help='render resolution WxH (overrides the preset)')
    p.add_argument('--print-preset-size',action='store_true',help='print the selected preset size, if reduced')
    p.add_argument('--output-res',default='',help='output resolution WxH (the upscaler\'s; the UI stays 1920x1080)')
    p.add_argument('--print-scaled',action='store_true',
                   help='print "RENDER OUTPUT HUD" (WxH) when bbport.ini selects an output other than 1080p '
                        '(HUD: the UI canvas, wider than 1920x1080 for ultrawide outputs)')
    a=p.parse_args()
    if a.print_scaled:
        sizes=scaled_sizes(read_settings(a.settings))
        if sizes:
            ui=hud_canvas(sizes[1])
            print(f'{sizes[0][0]}x{sizes[0][1]} {sizes[1][0]}x{sizes[1][1]} {ui[0]}x{ui[1]}')
        return
    if a.print_preset_size:
        settings=read_settings(a.settings)
        if 'BB_UPSCALER' in os.environ:
            settings['upscaler']='fsr3' if os.environ['BB_UPSCALER']=='fsr3' else 'off'
        if 'BB_UPSCALE_PRESET' in os.environ:
            settings['preset']=os.environ['BB_UPSCALE_PRESET']
        size=render_size(settings)
        if size: print(f'{size[0]}x{size[1]}')
        return
    if frame_hooks(a.fps):
        print(f'Patches: frame rate {a.fps} by hooks in the game code (BB_FRAME_HOOKS=0: the FPS++ patches)',file=sys.stderr)
    fps_patches = preset_patches(a.fps)
    raw_names = fps_patches + [n.strip() for n in a.extra.split(';') if n.strip()]
    raw_names += [n for n in effect_patches(read_settings(a.settings)) if n not in raw_names]
    if intel_tonemap_fix() and INTEL_TONEMAP not in raw_names:
        raw_names.append(INTEL_TONEMAP)
    available={m.get('Name') for m in ET.parse(a.xml).getroot().iter('Metadata') if m.get('AppVer')==a.app_version}
    choice=network_choice_patch(available=available)
    if party_mode() and choice==SKIP_NETWORK_CHOICE_ONLINE and SKIP_NETWORK_CHOICE in raw_names:
        # An explicit offline skip (BB_PATCHES) would boot a party member offline: the online one.
        print(f'Patches: party mode: "{SKIP_NETWORK_CHOICE}" -> "{SKIP_NETWORK_CHOICE_ONLINE}"',file=sys.stderr)
        raw_names=[SKIP_NETWORK_CHOICE_ONLINE if n==SKIP_NETWORK_CHOICE else n for n in raw_names]
    if choice and choice not in raw_names and not any(n in raw_names for n in (SKIP_NETWORK_CHOICE,SKIP_NETWORK_CHOICE_ONLINE)):
        raw_names.append(choice)
    raw_names += [n for n in party_patches() if n not in raw_names]
    if party_mode() and DISABLE_HTTP in raw_names:
        print(f'Patches: "{DISABLE_HTTP}" is off in party mode (BB_PARTY)',file=sys.stderr)
        raw_names = [n for n in raw_names if n!=DISABLE_HTTP]
    names = []
    for n in raw_names:
        if n not in names:
            names.append(n)
    names = validate_patch_requirements(names,a.game_dir)
    if frame_hooks(a.fps):
        # A built-in FPS patch (an FPS++ preset in BB_PATCHES, "60FPS (no deltatime)") or another
        # patch of the hooks' bytes: left out (the hooks check the game's bytes they rewrite).
        metas_all={m.get('Name'):m for m in ET.parse(a.xml).getroot().iter('Metadata') if m.get('AppVer')==a.app_version}
        names=without_frame_hook_conflicts(names,lambda n: metadata_writes(metas_all[n]) if n in metas_all else [],
                                           lambda n: f'"{n}"')
    elf=(a.out/'eboot.elf').read_bytes()
    segments=eboot_segments(elf)
    names=[n for n in names if originals_match(a.xml,n,a.app_version,elf)]
    # The FPS preset's writes come first, so an external FPS patch can replace them whole.
    fps_names=[n for n in names if n in fps_patches]
    names=fps_names+[n for n in names if n not in fps_names]
    fps_writes=compile_patches(a.xml,fps_names,a.app_version,segments)
    writes=fps_writes+compile_patches(a.xml,names[len(fps_names):],a.app_version,segments)
    metas={m.get('Name'):m for m in ET.parse(a.xml).getroot().iter('Metadata') if m.get('AppVer')==a.app_version}
    party_items=[(n,metas.get(n),compile_patches(a.xml,[n],a.app_version,segments)) for n in names]
    party=[n for n in names if n.startswith('Party: ')]
    if party:
        print(f'Patches: party co-op: {", ".join(party)} (original bytes checked)')
    size=render_size(read_settings(a.settings),a.render_res) if a.render_res else None
    # The UI keeps the game's 1920x1080 coordinates even for a larger output: the port draws
    # it into the output-size image with a viewport scaled by output / 1920
    # (UiComposition::NativeViewport), so it is rasterized at the output resolution. Ultrawide
    # outputs widen those coordinates (hud_canvas; run.bat passes the same size as BB_UI_RES).
    try:
        output=tuple(int(v) for v in a.output_res.lower().split('x')) if a.output_res else OUTPUT_SIZE
    except ValueError:
        output=OUTPUT_SIZE
    ui=hud_canvas(output) if size else OUTPUT_SIZE
    # bbport (from Mrsuss60/bloodborne_pc_windows_port): a 1920x1080 scene for a 1080p output (the
    # launcher's "1920x1080") is the game's own layout. The 1280x720 template would still write its
    # 720p-only constants (lock-on / HP bar coordinates, a scale factor): the executable is left
    # as it is instead. The upscaler treats this session as unscaled (TemporalUpscaler::Scaled).
    if size==OUTPUT_SIZE and a.output_res.lower() in ('','1920x1080'):
        print('Patches: scene 1920x1080 for a 1080p output: native layout, no resolution patch')
        size=None
    if size:
        writes+=resolution_writes(a.xml,size,a.app_version,segments,ui)
        if ui!=OUTPUT_SIZE:
            writes+=ultrawide_writes(a.xml,output,a.app_version,segments)
            names.append(f'ultrawide {output[0]}x{output[1]} (HUD {ui[0]}x{ui[1]})')
        if size[0]*size[1]>OUTPUT_SIZE[0]*OUTPUT_SIZE[1]:
            heap='Increased Graphics Heap Sizes'
            writes+=compile_patches(a.xml,[heap],a.app_version,segments)
            names.append(heap)
        print(f'Patches: scene {size[0]}x{size[1]}; UI {ui[0]}x{ui[1]}')
    if a.patches_dir:
        # After the built-in ones: an external patch of the same bytes wins.
        selected=external_selection(external_patches(a.patches_dir,a.app_version,a.xml),a.patches_config)
        if party_mode():
            dropped=[key for key,_,meta in selected if meta.get('Name')==DISABLE_HTTP]
            for key in dropped:
                print(f'Patches: external {key} is off in party mode (BB_PARTY)',file=sys.stderr)
            selected=[x for x in selected if x[0] not in dropped]
        if frame_hooks(a.fps):
            selected=without_frame_hook_conflicts(selected,lambda x: metadata_writes(x[2]),lambda x: f'external {x[0]}')
        applied=[]
        external=[]
        for item in selected:
            ours=compile_external([item],segments,applied)
            # An FPS patch of its own (e.g. "60FPS (no deltatime)", at 60 FPS++'s addresses with
            # other values) replaces the built-in preset whole: applied over it, the two mixed.
            if fps_writes and overlaps(fps_writes,ours):
                print(f'Patches: external {item[0]} replaces the built-in {", ".join(fps_names)}')
                writes=writes[len(fps_writes):]
                names=[n for n in names if n not in fps_names]
                party_items=[x for x in party_items if x[0] not in fps_names]
                fps_writes=[]
            external+=ours
        writes+=external
        party_items+=[(f'{meta.get("Name")} (external)',meta,ours) for _,meta,ours in applied]
    # BBPATCH2: the patch base, so the loader can rebase pointers the patches write into
    # relocated slots (60/90 FPS++ replace function pointers).
    blob=struct.pack('<8sQQ',b'BBPATCH2',EBOOT_BASE,len(writes))
    for offset,data in writes: blob+=struct.pack('<QQ',offset,len(data))+data
    (a.out/'patches.bin').write_bytes(blob)
    print(f'Patches: FPS preset {a.fps}; {len(writes)} writes from {names or "none"}')
    # The resolution writes and the heap patch are cosmetic: not in party_items.
    digest,gameplay=party_patch_set(party_items)
    cosmetic=sorted(n for n,meta,_ in party_items if patch_is_cosmetic(n,meta))
    write_party_patch_file(a.out,digest,gameplay,cosmetic)
    if party_mode():
        print(f'Patches: party version check: {len(gameplay)} gameplay patches ({digest[:12]}): '
              f'{", ".join(gameplay) or "none"}; not compared: {", ".join(cosmetic) or "none"}')


if __name__=='__main__':
    main()

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

EBOOT_BASE=0x400000
# BB_FPS presets: patch names from patches/Bloodborne.xml (app version 01.09).
FPS_PRESETS={'30':[],'60':['60 FPS++'],'90':['90 FPS++'],'uncap':['Uncap FPS++']}
# Upscaler presets (bbport.ini "preset", the in-game menu): output / render size ratio. The game
# then renders at 1920x1080 / ratio and the port's temporal upscaler restores the output size.
OUTPUT_SIZE=(1920,1080)
PRESET_SCALES=[1.0,1.5,1.7,2.0,3.0]
# The community patch changes two independent consumers: the game render/window setup
# and the UI movie viewport. Keep the latter at native size so glyph rasterisation and
# vector tessellation do not inherit the scene's FSR resolution.
RESOLUTION_TEMPLATE='Resolution Patch 1280x720 (16:9)'
# Outputs wider than 16:9: Kyo's 21:9 patch widens the camera's aspect ratio, the HUD canvas
# (1080 high, wider), the HUD projection and the screen-to-HUD mapping of the lock-on and HP-bar
# markers; ultrawide_writes recomputes those values for the output's own aspect ratio.
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
    # Cheats and gameplay tweaks (launcher "Cheats" page).
    'cheat_no_death':(None,'Player No Dead (Read note)'),
    'cheat_stealth':(None,'Player Stealth (Read note)'),
    'cheat_silent':(None,'Player Silent (Read note)'),
    'cheat_rally_no_decay':(None,'No Rally Decay'),
    'cheat_enemy_control':(None,'Enemy Control'),
    'tweak_no_rally':(None,'Disable Rally (HP Regain)'),
    'tweak_camera_distance':(None,'Increased camera distance'),
    'tweak_no_camera_rotation':(None,'Disable Camera Auto Rotation via Movement'),
    'tweak_easy_run':(None,'Sensitive Analog Input (easier to run)'),
    'tweak_ragdoll':(None,'DS1-like physics'),
    'tweak_circle_confirm':(None,'Circle Confirm (Japanese/Asian button layout)'),
}
# model_lod: -2 highest, 0 the game's, 1 lower, 2 lowest.
MODEL_LOD={'-2':'Model LOD -2 (Highest)','1':'Model LOD 1 (Lower)','2':'Model LOD 2 (Lowest)'}


def game_app_version(game):
    """APP_VER of the game folder's param.sfo ("01.09"), or None when it cannot be read."""
    try:
        from prepare import sfo
        return sfo((Path(game) / 'sce_sys/param.sfo').read_bytes()).get('APP_VER')
    except (OSError, ValueError, ImportError):
        return None


def validate_patch_requirements(names, game):
    if 'Restore Debug Camera' in names and 'Enemy Control' in names:
        raise ValueError('Restore Debug Camera conflicts with Enemy Control; enable only one')
    if 'Restore Debug Menu (READ NOTES)' in names:
        font = game / 'dvdroot_ps4/font'
        missing = [name for name in ('DbgFont14h.ccm', 'DbgFont14h.tpf')
                   if not (font / name).is_file() or (font / name).stat().st_size == 0]
        if missing:
            raise ValueError('Debug menu needs non-empty font files in '
                             f'{font}: {", ".join(missing)}. Install the fonts from '
                             'https://www.nexusmods.com/bloodborne/mods/253 first; '
                             'or disable debug_menu in bbport.ini')


def effect_patches(settings):
    names=[]
    for key,(off,on) in EFFECTS.items():
        if key not in settings: continue
        name=on if settings[key]=='1' else off
        if name: names.append(name)
    # Enemy Control and the free camera use the same buttons: the free camera wins.
    if 'Enemy Control' in names and 'Restore Debug Camera' in names:
        names.remove('Enemy Control')
        print('Patches: Enemy Control skipped (it conflicts with the free camera)')
    # Rally that never decays means nothing without Rally.
    if 'No Rally Decay' in names and 'Disable Rally (HP Regain)' in names:
        names.remove('No Rally Decay')
    lod=MODEL_LOD.get(settings.get('model_lod','0'))
    if lod: names.append(lod)
    return names
SCENE_WIDTH=0x02196A6B-EBOOT_BASE
SCENE_HEIGHT=0x02196A7A-EBOOT_BASE
UI_WIDTH=0x02358554-EBOOT_BASE
UI_HEIGHT=0x0235855D-EBOOT_BASE


def read_settings(path):
    # Path.read_text() defaults to the process locale; bbport.ini may contain
    # UTF-8 outside that encoding. Match the Windows launcher (encoding='utf-8').
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
        if 256<=w<=7680 and 144<=h<=4320 and not (w%2 or h%2): return (w,h)
    except ValueError:
        pass
    return OUTPUT_SIZE


def scaled_sizes(settings):
    """(render, output) for an output other than 1080p (above it, or 720p for the Steam Deck):
    the game renders at output / preset scale (or at the output size without upscaler) and the
    upscaler fills the output. None at 1080p and for TAA (native, live host targets only)."""
    out=output_size(settings)
    if out==OUTPUT_SIZE or (settings.get('upscaler')=='taa' and out[0]*9==out[1]*16): return None
    scale=1.0
    if settings.get('upscaler','fsr3') not in ('off','taa'):
        preset=int(settings.get('preset','0') or 0)
        scale=PRESET_SCALES[max(0,min(preset,len(PRESET_SCALES)-1))]
    render=tuple(max(2,round(v/scale/2)*2) for v in out)
    # A scene of exactly the game's 1920x1080 UI coordinate space (4K Performance) is
    # indistinguishable from a UI draw: the port's UI composition recognizes UI draws by that
    # size and the HUD gauges came out too long. The ultrawide HUD canvas (hud_canvas) is the
    # hud_layout mod layer's logical width only, never the guest UI space, so it does not matter
    # here (BB_UI_RES is the output size at every ratio, run.py/patches.py main()).
    if render==OUTPUT_SIZE:
        render=(OUTPUT_SIZE[0]-4,OUTPUT_SIZE[1]-2)
    elif out[0]*9!=out[1]*16 and render==out and settings.get('upscaler')!='taa':
        # Non-16:9 movie viewports use the physical output too. Native/off scene targets
        # otherwise alias the UI size and inventory draws sample the full icon atlas.
        # Keep TAA's exact-size contract; its native path cannot accept a render nudge.
        render=(out[0]-4,out[1]-2)
    return render,out


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


def hud_canvas(output):
    """The logical width the game's 1920x1080 stage has to grow to for an output, so its edges
    meet the output's own: 1920x1080 up to 16:9, wider (still 1080 high, even width) above it,
    e.g. 2580x1080 for 3440x1440. run.py passes the width to hud_layout.py, whose mod layer moves
    the HUD's corner groups toward the physical screen edges. It is never the guest UI coordinate
    space: BB_UI_RES is the output size for every ratio (centered-stage UiComposition)."""
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


def eboot_segments(elf):
    phoff,=struct.unpack_from('<Q',elf,0x20)
    phentsize,phnum=struct.unpack_from('<HH',elf,0x36)
    segments=[]
    for i in range(phnum):
        kind,_,_,vaddr,_,_,memsz,_=struct.unpack_from('<IIQQQQQQ',elf,phoff+i*phentsize)
        if kind==1: segments.append((vaddr,vaddr+memsz))
    return segments


def eboot_bytes(elf,start,end):
    """The eboot's memory image at [start, end): its PT_LOAD file bytes, zero past their file size."""
    phoff,=struct.unpack_from('<Q',elf,0x20)
    phentsize,phnum=struct.unpack_from('<HH',elf,0x36)
    out=bytearray(end-start)
    for i in range(phnum):
        kind,_,offset,vaddr,_,filesz,_,_=struct.unpack_from('<IIQQQQQQ',elf,phoff+i*phentsize)
        low,high=max(start,vaddr),min(end,vaddr+filesz)
        if kind==1 and low<high: out[low-start:high-start]=elf[offset+low-vaddr:offset+high-vaddr]
    return bytes(out)


# The patches write bytes into the code of one build of the game. A game whose param.sfo says 01.09
# but whose eboot.bin holds other code (a base-game eboot beside an update's param.sfo, a modified
# eboot) got them written over other instructions and crashed at start (write fault at guest
# offset 0x20348b8). The fingerprint covers the game's original bytes everywhere the FPS and
# resolution patches write, so another region's build with the same code passes. Recompute it
# with --print-fingerprint (after a run of CUSA03173 v01.09) when those patches change.
BUILTIN_XML=Path(__file__).resolve().parent.parent/'patches/Bloodborne.xml'
FINGERPRINT_PATCHES=sorted({n for names in FPS_PRESETS.values() for n in names}|{RESOLUTION_TEMPLATE})
PATCH_FINGERPRINTS={'01.09':'34e41d1df9e3805762bfd59054e69fe60c330b9b6df7906fa7f66a43c939fbf0'}


def patch_sites(xml,app_version,names=FINGERPRINT_PATCHES):
    """Sorted, merged [start, end) eboot ranges the named patches write."""
    ranges=[]
    for meta in ET.parse(xml).getroot().iter('Metadata'):
        if meta.get('Name') not in names or meta.get('AppVer')!=app_version: continue
        for line in meta.iter('Line'):
            offset=int(line.get('Address'),0)-EBOOT_BASE
            ranges.append((offset,offset+len(encode(line))))
    merged=[]
    for start,end in sorted(ranges):
        if merged and start<=merged[-1][1]: merged[-1]=(merged[-1][0],max(merged[-1][1],end))
        else: merged.append((start,end))
    return merged


def patch_fingerprint(elf,xml=BUILTIN_XML,app_version='01.09'):
    digest=hashlib.sha256()
    for start,end in patch_sites(xml,app_version):
        digest.update(struct.pack('<QQ',start,end)+eboot_bytes(elf,start,end))
    return digest.hexdigest()


def eboot_matches_patches(elf_path,app_version='01.09'):
    """False when eboot.elf (prepare.py) differs from the build the patches were made for."""
    expected=PATCH_FINGERPRINTS.get(app_version)
    return not expected or patch_fingerprint(Path(elf_path).read_bytes(),BUILTIN_XML,app_version)==expected


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
                raise ValueError(f'{name}: address {line.get("Address")} is outside the eboot')
            writes.append((offset,data))
    return writes


# Third-party patch files (shadPS4/GoldHEN XML) in the data directory's patches/ folder.
# Every retail release with update 1.09 (the same executable): US, EU, UK, JP, Asia, then the
# editions with The Old Hunters.
BLOODBORNE_IDS={'CUSA00900','CUSA00207','CUSA00208','CUSA00299','CUSA01363',
                'CUSA03179','CUSA03173','CUSA03014','CUSA03023'}


def external_patches(directory, app_version='01.09', exclude=Path(__file__).resolve().parent.parent/'patches/Bloodborne.xml'):
    """[(key, file, metadata)] of eboot patches for this version in directory/*.xml.
    key is "<file name>/<patch name>" (the launcher's selection, patches.json)."""
    found=[]
    directory=Path(directory)
    for path in sorted(directory.glob('*.xml')) if directory.is_dir() else []:
        if path.resolve()==Path(exclude).resolve(): continue  # the built-in file (patches/ in a checkout)
        # Left by bbbridge's app: play logs every 5 s to The Hunter's Dream.
        if path.name.lower()=='bbbridge-live-map.xml': continue
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


def compile_external(selected, segments):
    """Writes of the selected external patches; a patch with unsupported lines is skipped whole."""
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
    p.add_argument('--output-res',default='',help='output resolution WxH (centered 1920x1080 logical HUD stage)')
    p.add_argument('--print-scaled',action='store_true',
                   help='print "RENDER OUTPUT" (WxH) when bbport.ini selects an output other than 1080p')
    p.add_argument('--print-fingerprint',action='store_true',
                   help='print the patch fingerprint of <out>/eboot.elf (PATCH_FINGERPRINTS)')
    a=p.parse_args()
    if a.print_fingerprint:
        print(patch_fingerprint((a.out/'eboot.elf').read_bytes(),BUILTIN_XML,a.app_version))
        return
    if a.print_scaled:
        sizes=scaled_sizes(read_settings(a.settings))
        if sizes: print(f'{sizes[0][0]}x{sizes[0][1]} {sizes[1][0]}x{sizes[1][1]}')
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
    # The patches are byte writes at the addresses of one game version: on another version they
    # would corrupt code. Such a game runs unpatched (run.sh/run.py then choose 30 FPS).
    version=game_app_version(a.game_dir)
    force=os.environ.get('BB_FORCE_PATCHES')
    if version and version!=a.app_version and not force:
        (a.out/'patches.bin').write_bytes(struct.pack('<8sQQ',b'BBPATCH2',EBOOT_BASE,0))
        print(f'Patches: game version {version}, patches are for {a.app_version}: none applied '
              '(30 FPS, no effect or resolution patches)')
        return
    if not force and not eboot_matches_patches(a.out/'eboot.elf',a.app_version):
        (a.out/'patches.bin').write_bytes(struct.pack('<8sQQ',b'BBPATCH2',EBOOT_BASE,0))
        print(f'Patches: eboot.bin is not the {a.app_version} build the patches were made for: none '
              'applied (30 FPS, no effect or resolution patches; BB_FORCE_PATCHES=1 applies them)')
        return
    names=FPS_PRESETS[a.fps]+[n.strip() for n in a.extra.split(';') if n.strip()]
    names+=[n for n in effect_patches(read_settings(a.settings)) if n not in names]
    validate_patch_requirements(names,a.game_dir)
    segments=eboot_segments((a.out/'eboot.elf').read_bytes())
    writes=compile_patches(a.xml,names,a.app_version,segments)
    size=render_size(read_settings(a.settings),a.render_res) if a.render_res else None
    # Scaleform's logical stage remains 1920x1080. At non-16:9 output sizes,
    # give its show-all mode the actual physical viewport so it centers the stage.
    # The renderer uses that coordinate space for native-resolution UI draws.
    # The HUD's own corner groups are moved by the hud_layout mod layer (run.py,
    # hud_canvas width), not by these writes: the camera stays at the output aspect.
    ui=OUTPUT_SIZE
    if a.output_res:
        out=output_size({'output_res': a.output_res})
        if out[0]*9 != out[1]*16: ui=out
    if size:
        writes+=resolution_writes(a.xml,size,a.app_version,segments,ui)
        if size[0]*size[1]>OUTPUT_SIZE[0]*OUTPUT_SIZE[1]:
            heap='Increased Graphics Heap Sizes'
            writes+=compile_patches(a.xml,[heap],a.app_version,segments)
            names.append(heap)
        print(f'Patches: scene {size[0]}x{size[1]}; UI {ui[0]}x{ui[1]}')
    if read_settings(a.settings).get('quit_to_desktop','1')!='0' and os.environ.get('BB_QUIT_TO_DESKTOP')!='0':
        # Quit to Desktop in the title and System menus (quit_menu.py; its texts are a mod layer).
        sys.path.insert(0,str(Path(__file__).resolve().parent))
        import quit_menu
        elf=(a.out/'eboot.elf').read_bytes()
        try:
            quit_menu.check_original(lambda start,end: eboot_bytes(elf,start,end))
            writes+=quit_menu.code_writes()
            names.append('Quit to Desktop')
        except ValueError as error:
            print(f'Patches: {error}; Quit to Desktop skipped')
    if a.patches_dir:
        # After the built-in ones: an external patch of the same bytes wins.
        writes+=compile_external(external_selection(external_patches(a.patches_dir,a.app_version,a.xml),
                                                    a.patches_config),segments)
    # BBPATCH2: the patch base, so the loader can rebase pointers the patches write into
    # relocated slots (60/90 FPS++ replace function pointers).
    blob=struct.pack('<8sQQ',b'BBPATCH2',EBOOT_BASE,len(writes))
    for offset,data in writes: blob+=struct.pack('<QQ',offset,len(data))+data
    (a.out/'patches.bin').write_bytes(blob)
    print(f'Patches: FPS preset {a.fps}; {len(writes)} writes from {names or "none"}')


if __name__=='__main__':
    main()

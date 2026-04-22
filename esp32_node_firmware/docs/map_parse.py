import re, collections

iram_by_lib  = collections.defaultdict(int)
flash_by_lib = collections.defaultdict(int)
dram_by_lib  = collections.defaultdict(int)

SECTION_MAP = {
    '.iram0.text':   iram_by_lib,
    '.flash.text':   flash_by_lib,
    '.flash.rodata': flash_by_lib,
    '.dram0.data':   dram_by_lib,
    '.dram0.bss':    dram_by_lib,
}

def lib_name(path):
    p = path.replace('\\', '/')
    # Archive member  e.g. libArduinoJson.a(...)
    m = re.search(r'/([^/]+\.a)\(', p)
    if m:
        name = re.sub(r'^lib', '', m.group(1))
        return re.sub(r'\.a$', '', name)
    # PlatformIO short lib dirs  e.g. .pio/build/esp32dev/lib0c7/FastLED/...
    m = re.search(r'/lib[0-9a-f]{3}/([^/]+)/', p)
    if m:
        return m.group(1)
    # libFrameworkArduino.a/...
    m = re.search(r'libFrameworkArduino\.a', p)
    if m:
        return 'Arduino-core'
    # Catch-all for SDK objects
    if 'framework-arduinoespressif32-libs' in p or 'toolchain-xtensa' in p:
        return 'SDK/IDF-prebuilt'
    parts = [x for x in p.split('/') if x]
    return parts[-2] if len(parts) >= 2 else (parts[-1] if parts else 'unknown')

current_section = None
MAP_PATH = 'C:/Users/drowa/Documents/git/Arduino/NodeFirmware/esp32_node_firmware/.pio/build/esp32dev/firmware.map'

with open(MAP_PATH, 'r', errors='replace') as f:
    for line in f:
        sec_m = re.match(r'^(\.[a-z0-9_.]+)\s+0x', line)
        if sec_m:
            s = sec_m.group(1)
            current_section = s if s in SECTION_MAP else None
            continue
        if not current_section:
            continue
        # 3-token: " .sectionname 0xADDR 0xSIZE path"  (input-section listing)
        m = re.match(r'^\s+\S+\s+0x[0-9a-f]+\s+(0x[0-9a-f]+)\s+(.+)$', line)
        # 2-token: "        0xADDR 0xSIZE path"  (output-section allocations)
        # [^(] excludes "(size before relaxing)" continuation lines
        if not m:
            m = re.match(r'^\s+0x[0-9a-f]+\s+(0x[0-9a-f]+)\s+([^(].+)$', line)
        if not m:
            continue
        size = int(m.group(1), 16)
        if size == 0:
            continue
        SECTION_MAP[current_section][lib_name(m.group(2).strip())] += size

def print_table(title, d, cap):
    print('\n' + '='*70)
    print(f'  {title}   (capacity {cap/1024:.0f} KB = {cap} B)')
    print(f'  {"Component":<40} {"Bytes":>8}  {"KB":>6}  {"% cap":>6}')
    print('  ' + '-'*62)
    shown = 0
    for k, v in sorted(d.items(), key=lambda x: -x[1]):
        if v < 200:
            continue
        print(f'  {k:<40} {v:>8}  {v/1024:>6.1f}  {v*100/cap:>5.1f}%')
        shown += v
    other = sum(d.values()) - shown
    if other > 0:
        print(f'  {"(small objects < 200 B)":<40} {other:>8}  {other/1024:>6.1f}  {other*100/cap:>5.1f}%')
    total = sum(d.values())
    print('  ' + '-'*62)
    print(f'  {"TOTAL":<40} {total:>8}  {total/1024:>6.1f}  {total*100/cap:>5.1f}%')

print_table('IRAM (.iram0.text)',                 iram_by_lib,  132*1024)
print_table('FLASH (.flash.text + .flash.rodata)',flash_by_lib, 1920*1024)
print_table('DRAM (.dram0.data + .dram0.bss)',    dram_by_lib,  122*1024)

print('\n' + '='*70)
print('  Ground-truth section sizes (from linker section headers):')
print('    .iram0.text :  131363 B =  128.3 KB  (capacity 132 KB = 0x20000+0x800)')
print('    .flash.text :  1218928 B = 1190.4 KB')
print('    .flash.rodata: 325628 B =  318.0 KB')
print('    FLASH total :  1544556 B = 1508.4 KB  (capacity 1920 KB = 78.6%)')
print('    .dram0.data :   28153 B =   27.5 KB')
print('    .dram0.bss  :   52912 B =   51.7 KB')
print('    DRAM static :   81065 B =   79.2 KB  (capacity 122 KB = 64.8%)')
print()
print('  Note: flash.rodata breakdown totals may be ~50% overcounted due to')
print('  merged string-literal sections (.rodata.*.str1.1) in the map file.')
print('  The linker keeps one merged copy, but each contributing object')
print('  appears at the same address, inflating per-library byte counts.')
print('  IRAM and DRAM totals are accurate (no string-merging in those regions).')

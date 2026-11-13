"""Isolated Linux playback memory profile; never invokes the regression suite."""
import argparse
import csv
import functools
import http.server
import json
import os
from pathlib import Path
import shutil
import statistics
import struct
import subprocess
import threading
import time
import zlib

if os.name == 'nt':
    import ctypes
    from ctypes import wintypes
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    psapi = ctypes.WinDLL('psapi', use_last_error=True)
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    psapi.GetProcessMemoryInfo.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD]
    psapi.QueryWorkingSet.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD]
    psapi.EnumProcessModulesEx.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), wintypes.DWORD]
    psapi.GetModuleFileNameExW.argtypes = [wintypes.HANDLE, wintypes.HMODULE, wintypes.LPWSTR, wintypes.DWORD]
    class MemoryCounters(ctypes.Structure):
        _fields_ = [('cb', wintypes.DWORD), ('PageFaultCount', wintypes.DWORD)] + [
            (name, ctypes.c_size_t) for name in ('PeakWorkingSetSize', 'WorkingSetSize', 'QuotaPeakPagedPoolUsage',
                'QuotaPagedPoolUsage', 'QuotaPeakNonPagedPoolUsage', 'QuotaNonPagedPoolUsage', 'PagefileUsage', 'PeakPagefileUsage', 'PrivateUsage')]
    class ProcessEntry(ctypes.Structure):
        _fields_ = [('dwSize', wintypes.DWORD), ('cntUsage', wintypes.DWORD), ('th32ProcessID', wintypes.DWORD),
            ('th32DefaultHeapID', ctypes.c_size_t), ('th32ModuleID', wintypes.DWORD), ('cntThreads', wintypes.DWORD),
            ('th32ParentProcessID', wintypes.DWORD), ('pcPriClassBase', wintypes.LONG), ('dwFlags', wintypes.DWORD),
            ('szExeFile', wintypes.WCHAR * 260)]
    kernel.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
    kernel.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    kernel.Process32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(ProcessEntry)]
    kernel.Process32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(ProcessEntry)]
    kernel.GetProcessTimes.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.FILETIME), ctypes.POINTER(wintypes.FILETIME),
        ctypes.POINTER(wintypes.FILETIME), ctypes.POINTER(wintypes.FILETIME)]


def windows_created(pid):
    handle = kernel.OpenProcess(0x1000, False, pid)
    if not handle:
        return None
    try:
        values = [wintypes.FILETIME() for _ in range(4)]
        if not kernel.GetProcessTimes(handle, *(ctypes.byref(value) for value in values)):
            return None
        return (values[0].dwHighDateTime << 32) | values[0].dwLowDateTime
    finally:
        kernel.CloseHandle(handle)


def windows_memory(pid):
    handle = kernel.OpenProcess(0x0400 | 0x0010, False, pid)
    if not handle:
        return None
    try:
        counters = MemoryCounters(); counters.cb = ctypes.sizeof(counters)
        if not psapi.GetProcessMemoryInfo(handle, ctypes.byref(counters), counters.cb):
            return None
        # Shared=0 统计不可共享驻留页；PrivateUsage 另列私有提交。两者也不等同于 Linux Private_Clean。
        count = counters.WorkingSetSize // 4096 + 4096
        for _ in range(3):
            pages = (ctypes.c_size_t * (count + 1))()
            if psapi.QueryWorkingSet(handle, pages, ctypes.sizeof(pages)):
                break
            count = max(count * 2, int(pages[0]) + 4096)
        else:
            raise ctypes.WinError(ctypes.get_last_error())
        private_kib = sum(not (pages[i] & (1 << 8)) for i in range(1, int(pages[0]) + 1)) * 4
        return {'VmRSS': counters.WorkingSetSize // 1024, 'VmHWM': counters.PeakWorkingSetSize // 1024,
                'Private': private_kib, 'PrivateCommit': counters.PrivateUsage // 1024, 'Pss': 0}
    finally:
        kernel.CloseHandle(handle)


def windows_children(pid):
    snapshot = kernel.CreateToolhelp32Snapshot(2, 0)
    if snapshot == ctypes.c_void_p(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    parents = {}
    try:
        entry = ProcessEntry(); entry.dwSize = ctypes.sizeof(entry)
        exists = kernel.Process32FirstW(snapshot, ctypes.byref(entry))
        while exists:
            parents[int(entry.th32ProcessID)] = int(entry.th32ParentProcessID)
            exists = kernel.Process32NextW(snapshot, ctypes.byref(entry))
    finally:
        kernel.CloseHandle(snapshot)
    found = {pid}; created = windows_created(pid)
    while True:
        more = {child for child, parent in parents.items() if parent in found and
                (born := windows_created(child)) is not None and created is not None and born >= created}
        if more <= found:
            return sorted(found - {pid})
        found |= more


def windows_modules(pid):
    handle = kernel.OpenProcess(0x0400 | 0x0010, False, pid)
    if not handle:
        return []
    try:
        modules = (wintypes.HMODULE * 2048)(); needed = wintypes.DWORD()
        if not psapi.EnumProcessModulesEx(handle, modules, ctypes.sizeof(modules), ctypes.byref(needed), 3):
            raise ctypes.WinError(ctypes.get_last_error())
        result = []
        for module in modules[:min(len(modules), needed.value // ctypes.sizeof(wintypes.HMODULE))]:
            name = ctypes.create_unicode_buffer(32768)
            if psapi.GetModuleFileNameExW(handle, module, name, len(name)):
                result.append(name.value)
        return result
    finally:
        kernel.CloseHandle(handle)


def prepare(source):
    source = Path(source)
    main = source / 'src/main.cpp'
    text = main.read_text(encoding='utf-8')
    replacements = {
        '#include "ui.hpp"': '#include "ui.hpp"\n#include "memory_probe.hpp"',
        '    using namespace player;': '    using namespace player;\n    if (argc > 1 && std::string(argv[1]) == "--memory-assets") return memory_probe::assets();',
        'if (smoke_test) initial_search = false;': 'initial_search = false; smoke_test = false;',
        'initialize(smoke_test)': 'initialize(true)',
        'bool autostart = true;': 'memory_probe::initialize(win);\n    bool autostart = true;',
        'bootstrap(initial_search, smoke_test)': 'bootstrap(false, true)',
        '        tick();': '        memory_probe::step(win);\n        tick();',
        '        ui::draw(ui, model);': '        ui::draw(ui, model);\n        memory_probe::rendered();',
        '    shutdown(smoke_test);': '    memory_probe::shutdown();\n    shutdown(true);\n    if (memory_probe::failed) return 2;',
    }
    for old, new in replacements.items():
        if text.count(old) != 1:
            raise RuntimeError(f'Expected exactly one injection point: {old!r}')
        text = text.replace(old, new)
    main.write_text(text, encoding='utf-8')
    shutil.copy2(Path(__file__).with_name('memory_probe.hpp'), source / 'src/memory_probe.hpp')
    cover = source / 'src/cover.hpp'
    text = cover.read_text(encoding='utf-8')
    anchor = 'class Store {\npublic:'
    assert text.count(anchor) == 1
    text = text.replace(anchor, anchor + '''
    std::pair<size_t, size_t> memory_profile_stats() {
        std::lock_guard<std::mutex> lock(m_); size_t bytes = 0;
        for (const auto& entry : map_) if (entry.second.img) bytes += entry.second.img->rgba.size();
        return {map_.size(), bytes};
    }
''')
    cover.write_text(text, encoding='utf-8')
    # 仅在临时诊断源中选择相同的实时 null 音频设备；两个系统仍执行完整消费回调。
    audio = source / 'src/audio.cpp'
    text = audio.read_text(encoding='utf-8')
    anchor = '        auto config = ma_engine_config_init();'
    assert text.count(anchor) == 2
    text = text.replace(anchor, anchor + '''
        static ma_context context;
        static const ma_result initialized = [] {
            const ma_backend backend = ma_backend_null;
            return ma_context_init(&backend, 1, nullptr, &context);
        }();
        if (initialized != MA_SUCCESS) { p.error = "diagnostic null audio context failed"; return false; }
        config.pContext = &context;
''')
    audio.write_text(text, encoding='utf-8')
    framework = source / 'src/aino.hpp'
    text = framework.read_text(encoding='utf-8')
    anchor = 'inline SystemIconMask load_system_icon(SystemIcon icon, int size) {\n    SystemIconMask mask;'
    assert text.count(anchor) == 1
    text = text.replace(anchor, anchor + '\n    if (std::getenv("AINO_MEMORY_NO_ICONS")) return mask;')
    # 诊断消融实验只屏蔽图标；不得把它当作可以接受的产品修复。
    framework.write_text(text, encoding='utf-8')


def read_memory(pid):
    if os.name == 'nt':
        return windows_memory(pid)
    result = {}
    try:
        for file in ('status', 'smaps_rollup'):
            for line in Path(f'/proc/{pid}/{file}').read_text().splitlines():
                key, _, value = line.partition(':')
                if key in ('VmRSS', 'VmHWM', 'RssAnon', 'RssFile', 'Threads', 'Pss', 'Private_Clean', 'Private_Dirty', 'Private_Hugetlb'):
                    result[key] = int(value.split()[0])
    except (FileNotFoundError, ProcessLookupError):
        return None
    result['Private'] = sum(result.get(key, 0) for key in ('Private_Clean', 'Private_Dirty', 'Private_Hugetlb'))
    return result


def descendants(pid):
    if os.name == 'nt':
        return windows_children(pid)
    children = []
    try:
        # 子进程可由任意解码工作线程创建，不能只读主线程的 children。
        for task in Path(f'/proc/{pid}/task').iterdir():
            try:
                children.extend(int(value) for value in (task / 'children').read_text().split())
            except FileNotFoundError:
                pass
    except FileNotFoundError:
        return []
    result = set(children)
    for child in children:
        result.update(descendants(child))
    return sorted(result)


def fixtures(root):
    root.mkdir(parents=True, exist_ok=True)
    for name, codec, extra in [('fixture.mp3', 'libmp3lame', ['-b:a', '192k']), ('fixture.aac', 'aac', ['-b:a', '192k', '-f', 'adts'])]:
        subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y', '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000:duration=120',
                        '-ac', '2', '-c:a', codec, *extra, str(root / name)], check=True)
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    pixels = b''.join(b'\0' + bytes((x * 7 + y * 11) % 256 for x in range(192 * 3)) for y in range(192))
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 192, 192, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(pixels)) + chunk(b'IEND', b'')


def run(binary, output, scale, fixture_directory=None, no_icons=False):
    output = Path(output).resolve(); output.mkdir(parents=True, exist_ok=True)
    root = output / 'fixtures'
    if fixture_directory:
        shutil.copytree(fixture_directory, root)
        png = (root / 'cover.png').read_bytes()
    else:
        png = fixtures(root)
    class Handler(http.server.SimpleHTTPRequestHandler):
        def do_GET(self):
            if self.path.startswith('/cover/'):
                self.send_response(200); self.send_header('Content-Length', str(len(png))); self.end_headers(); self.wfile.write(png)
            else:
                super().do_GET()
        def log_message(self, *_):
            pass
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), functools.partial(Handler, directory=str(root)))
    thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()
    env = dict(os.environ)
    for key in ('HOME', 'APPDATA', 'XDG_CONFIG_HOME', 'XDG_CACHE_HOME', 'TMPDIR', 'TEMP', 'TMP'):
        directory = output / key.lower(); directory.mkdir(); env[key] = str(directory)
    env.update(AINO_MEMORY_ROOT=str(root), AINO_MEMORY_URL=f'http://127.0.0.1:{server.server_port}', AINO_SCALE=str(scale), PULSE_SINK='aino_memory')
    if no_icons:
        env['AINO_MEMORY_NO_ICONS'] = '1'
    rows = []; traces = []; last_phase = ''; sampled_phases = set(); startup = []; first_frame = []
    with (output / 'player.log').open('w', encoding='utf-8') as log:
        process = subprocess.Popen([str(Path(binary).resolve())], env=env, stdout=subprocess.PIPE, stderr=log, text=True, encoding='utf-8', errors='replace')
        started = time.monotonic()
        def consume():
            for line in process.stdout:
                log.write(line); log.flush()
                if line.startswith('AINO_MEMORY '):
                    traces.append(json.loads(line[len('AINO_MEMORY '):]))
                elif line.startswith('AINO_FIRST_FRAME'):
                    first_frame.append(time.monotonic() - started)
        reader = threading.Thread(target=consume, daemon=True); reader.start()
        def sample_startup():
            while process.poll() is None and time.monotonic() - started < 4:
                parent = read_memory(process.pid)
                if parent:
                    children = [value for pid in descendants(process.pid) if (value := read_memory(pid))]
                    startup.append(dict(seconds=time.monotonic() - started, parent_rss_kib=parent['VmRSS'],
                        tree_rss_kib=parent['VmRSS'] + sum(value['VmRSS'] for value in children),
                        tree_pss_kib=parent['Pss'] + sum(value['Pss'] for value in children), children=len(children)))
                time.sleep(.02)
        startup_reader = threading.Thread(target=sample_startup, daemon=True); startup_reader.start()
        try:
            while process.poll() is None:
                if time.monotonic() - started > 75:
                    raise TimeoutError('memory fixture did not finish within 75 seconds')
                if traces:
                    trace = dict(traces[-1]); mem = read_memory(process.pid)
                    if mem:
                        row = dict(trace, **mem)
                        child_memory = []
                        details = []
                        for pid in descendants(process.pid):
                            value = read_memory(pid)
                            if value:
                                child_memory.append(value)
                                if os.name == 'nt':
                                    modules = windows_modules(pid)
                                    name = modules[0] if modules else str(pid)
                                else:
                                    try: name = os.readlink(f'/proc/{pid}/exe')
                                    except (FileNotFoundError, ProcessLookupError): name = str(pid)
                                details.append(dict(pid=pid, name=name, **value))
                        row['child_processes'] = details
                        for key in ('VmRSS', 'Pss', 'Private', 'PrivateCommit'):
                            row['children_' + key] = sum(value.get(key, 0) for value in child_memory)
                            row['tree_' + key] = row.get(key, 0) + row['children_' + key]
                        rows.append(row)
                        if trace['phase'] != last_phase:
                            last_phase = trace['phase']; print(f'{output.name}: {last_phase}', flush=True)
                        # 各场景结束前保留完整内存映射，供后续归因。
                        phase = trace['phase']
                        if phase not in sampled_phases or int(trace['seconds'] * 2) % 4 == 0:
                            if os.name == 'nt':
                                (output / f'{phase}-modules.json').write_text(json.dumps(windows_modules(process.pid), indent=2))
                            else:
                                (output / f'{phase}-smaps.txt').write_text(Path(f'/proc/{process.pid}/smaps').read_text())
                            sampled_phases.add(phase)
                time.sleep(.5)
            reader.join(timeout=5)
            if process.returncode:
                raise RuntimeError(f'player exited {process.returncode}; see {output}/player.log')
        finally:
            if process.poll() is None:
                process.terminate()
                try: process.wait(timeout=10)
                except subprocess.TimeoutExpired: process.kill(); process.wait()
            server.shutdown(); server.server_close(); thread.join()
            startup_reader.join(timeout=5)
            (output / 'startup.json').write_text(json.dumps({'first_frame_seconds': first_frame[0] if first_frame else None,
                'samples': startup}, indent=2), encoding='utf-8')
            (output / 'samples.json').write_text(json.dumps(rows, indent=2), encoding='utf-8')
            if rows:
                with (output / 'samples.csv').open('w', newline='') as csvfile:
                    writer = csv.DictWriter(csvfile, fieldnames=list(rows[0])); writer.writeheader(); writer.writerows(rows)
    for phase in ('local_mp3_lyrics', 'http_aac_lyrics', 'cover_pressure'):
        active = [r for r in rows if r['phase'] == phase and r['playing']]
        if len(active) < 5 or max(r['position_ms'] for r in active) - min(r['position_ms'] for r in active) < 2000:
            raise RuntimeError(f'{phase}: no sustained playback; memory data must not be presented as successful playback')
    if not rows or not all(abs(r['scale'] - scale) < .01 for r in rows):
        raise RuntimeError('actual window DPI does not match requested scale')
    if not all(r['physical_w'] == int(1180 * scale) and r['physical_h'] == int(760 * scale) for r in rows):
        raise RuntimeError('physical window dimensions do not match')
    (output / 'validated.json').write_text(json.dumps({'platform': os.name, 'audio': 'miniaudio null realtime device', 'scale': scale,
        'icons_disabled_for_diagnosis': no_icons, 'sustained_playback': True, 'samples': len(rows)}))


def report(root):
    lines = ['# Cross-platform playback memory attribution', '',
             'Identical generated audio, 1180x760 logical window, lyrics and miniaudio realtime null device.',
             'MiB. Linux Private_Clean+Private_Dirty and Windows nonshareable working-set pages have different sharing rules; neither is Windows private commit.',
             'Tree RSS/working-set sums include shared-page double counting. Windows PSS is unavailable. no-icons is a diagnostic ablation, not a product fix.', '',
             '| Variant | Phase | Parent RSS median / peak | OS-specific private resident median | Windows private commit median | Decoder + host RSS median | Tree RSS median / peak |',
             '|---|---|---:|---:|---:|---:|---:|']
    summary = {}
    for path in sorted(Path(root).glob('*/samples.json')):
        rows = json.loads(path.read_text(encoding='utf-8')); label = path.parent.name
        if not (path.parent / 'validated.json').exists():
            lines.append(f'INVALID: {label}'); continue
        summary[label] = {}
        for phase in ('idle', 'local_mp3_lyrics', 'http_aac_lyrics', 'cover_pressure', 'settled'):
            samples = [r for r in rows if r['phase'] == phase]
            stable = [r for r in samples if r['seconds'] >= samples[0]['seconds'] + 2] or samples
            keys = ('VmRSS', 'Private', 'children_VmRSS', 'tree_VmRSS')
            if 'PrivateCommit' in stable[0]: keys += ('PrivateCommit',)
            if stable[0].get('Pss', 0): keys += ('Pss', 'tree_Pss')
            values = {key + '_median_mib': statistics.median(r[key] for r in stable) / 1024 for key in keys}
            values.update(rss_peak_mib=max(r['VmRSS'] for r in samples) / 1024,
                          tree_rss_peak_mib=max(r['tree_VmRSS'] for r in samples) / 1024,
                          cover_end_mib=samples[-1]['cover_bytes'] / 1048576,
                          buffer_mib=samples[-1]['window_buffer_bytes'] / 1048576,
                          buffer_type=samples[-1]['window_buffer_type'])
            summary[label][phase] = values
            commit = f"{values['PrivateCommit_median_mib']:.2f}" if 'PrivateCommit_median_mib' in values else 'n/a'
            lines.append(f"| {label} | {phase} | {values['VmRSS_median_mib']:.2f} / {values['rss_peak_mib']:.2f} | {values['Private_median_mib']:.2f} | {commit} | {values['children_VmRSS_median_mib']:.2f} | {values['tree_VmRSS_median_mib']:.2f} / {values['tree_rss_peak_mib']:.2f} |")
    Path(root, 'summary.json').write_text(json.dumps(summary, indent=2), encoding='utf-8')
    Path(root, 'summary.md').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    print('\n'.join(lines))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__); commands = parser.add_subparsers(dest='command', required=True)
    p = commands.add_parser('prepare'); p.add_argument('source')
    p = commands.add_parser('run'); p.add_argument('binary'); p.add_argument('output'); p.add_argument('--scale', type=float, required=True)
    p.add_argument('--fixtures'); p.add_argument('--no-icons', action='store_true')
    p = commands.add_parser('fixtures'); p.add_argument('directory')
    p = commands.add_parser('report'); p.add_argument('root')
    args = parser.parse_args()
    if args.command == 'prepare': prepare(args.source)
    elif args.command == 'run': run(args.binary, args.output, args.scale, args.fixtures, args.no_icons)
    elif args.command == 'fixtures':
        target = Path(args.directory); png = fixtures(target); (target / 'cover.png').write_bytes(png)
    else: report(args.root)

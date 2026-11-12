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


def prepare(source):
    source = Path(source)
    main = source / 'src/main.cpp'
    text = main.read_text(encoding='utf-8')
    replacements = {
        '#include "ui.hpp"': '#include "ui.hpp"\n#include "memory_probe.hpp"',
        'if (smoke_test) initial_search = false;': 'initial_search = false; smoke_test = false;',
        'initialize(smoke_test)': 'initialize(true)',
        'bool autostart = true;': 'memory_probe::initialize(win);\n    bool autostart = true;',
        'bootstrap(initial_search, smoke_test)': 'bootstrap(false, true)',
        '        tick();': '        memory_probe::step(win);\n        tick();',
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


def read_memory(pid):
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


def run(binary, output, scale):
    output = Path(output).resolve(); output.mkdir(parents=True, exist_ok=True)
    root = output / 'fixtures'; png = fixtures(root)
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
    for key in ('HOME', 'XDG_CONFIG_HOME', 'XDG_CACHE_HOME', 'TMPDIR'):
        directory = output / key.lower(); directory.mkdir(); env[key] = str(directory)
    env.update(AINO_MEMORY_ROOT=str(root), AINO_MEMORY_URL=f'http://127.0.0.1:{server.server_port}', AINO_SCALE=str(scale), PULSE_SINK='aino_memory')
    rows = []; traces = []; last_phase = ''; sampled_phases = set()
    with (output / 'player.log').open('w', encoding='utf-8') as log:
        process = subprocess.Popen([str(Path(binary).resolve())], env=env, stdout=subprocess.PIPE, stderr=log, text=True, encoding='utf-8', errors='replace')
        def consume():
            for line in process.stdout:
                log.write(line); log.flush()
                if line.startswith('AINO_MEMORY '):
                    traces.append(json.loads(line[len('AINO_MEMORY '):]))
        reader = threading.Thread(target=consume, daemon=True); reader.start()
        started = time.monotonic()
        try:
            while process.poll() is None:
                if time.monotonic() - started > 75:
                    raise TimeoutError('memory fixture did not finish within 75 seconds')
                if traces:
                    trace = dict(traces[-1]); mem = read_memory(process.pid)
                    if mem:
                        row = dict(trace, **mem)
                        child_memory = [value for pid in descendants(process.pid) if (value := read_memory(pid))]
                        for key in ('VmRSS', 'Pss', 'Private'):
                            row['children_' + key] = sum(value.get(key, 0) for value in child_memory)
                            row['tree_' + key] = row.get(key, 0) + row['children_' + key]
                        rows.append(row)
                        if trace['phase'] != last_phase:
                            last_phase = trace['phase']; print(f'{output.name}: {last_phase}', flush=True)
                        # 各场景结束前保留完整内存映射，供后续归因。
                        phase = trace['phase']
                        if phase not in sampled_phases or int(trace['seconds'] * 2) % 4 == 0:
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
    (output / 'validated.json').write_text(json.dumps({'scale': scale, 'sustained_playback': True, 'samples': len(rows)}))


def report(root):
    lines = ['# Linux playback memory profile', '', 'Ubuntu 22.04 / Xvfb, 1180×760 logical window; PulseAudio software sink; identical fixtures and lyrics.',
             'All values below are MiB. Parent = player process; tree = player + decoder descendants. This is not a desktop Wayland measurement.', '',
             '| Build / scale | Phase | Parent RSS median / peak | Parent PSS median | Parent private median | Tree PSS median / peak | Cover pixels at end |',
             '|---|---|---:|---:|---:|---:|---:|']
    summary = {}
    for path in sorted(Path(root).glob('*/samples.json')):
        rows = json.loads(path.read_text()); label = path.parent.name
        if not (path.parent / 'validated.json').exists():
            lines.append(f'\n**{label}: INVALID / playback not validated.**'); continue
        summary[label] = {}
        for phase in ('idle', 'local_mp3_lyrics', 'http_aac_lyrics', 'cover_pressure', 'settled'):
            samples = [r for r in rows if r['phase'] == phase]
            # 排除每阶段最初两秒的切换与分配峰值，但另保留全阶段 peak。
            stable = [r for r in samples if r['seconds'] >= samples[0]['seconds'] + 2] or samples
            values = {key + '_median_mib': statistics.median(r[key] for r in stable) / 1024 for key in ('VmRSS', 'Pss', 'Private', 'tree_Pss')}
            values.update(rss_peak_mib=max(r['VmRSS'] for r in samples) / 1024, tree_pss_peak_mib=max(r['tree_Pss'] for r in samples) / 1024,
                          cover_end_mib=samples[-1]['cover_bytes'] / 1048576, glyph_end_mib=samples[-1]['glyph_bytes'] / 1048576,
                          tasks_end=samples[-1]['background_tasks'])
            summary[label][phase] = values
            lines.append(f'| {label} | {phase} | {values["VmRSS_median_mib"]:.2f} / {values["rss_peak_mib"]:.2f} | {values["Pss_median_mib"]:.2f} | {values["Private_median_mib"]:.2f} | {values["tree_Pss_median_mib"]:.2f} / {values["tree_pss_peak_mib"]:.2f} | {values["cover_end_mib"]:.2f} |')
    Path(root, 'summary.json').write_text(json.dumps(summary, indent=2))
    Path(root, 'summary.md').write_text('\n'.join(lines) + '\n')
    print('\n'.join(lines))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__); commands = parser.add_subparsers(dest='command', required=True)
    p = commands.add_parser('prepare'); p.add_argument('source')
    p = commands.add_parser('run'); p.add_argument('binary'); p.add_argument('output'); p.add_argument('--scale', type=float, required=True)
    p = commands.add_parser('report'); p.add_argument('root')
    args = parser.parse_args()
    if args.command == 'prepare': prepare(args.source)
    elif args.command == 'run': run(args.binary, args.output, args.scale)
    else: report(args.root)

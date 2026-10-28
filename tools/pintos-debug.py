#!/usr/bin/env python3
"""VS Code lifecycle for this repository's existing Pintos test recipes."""
import argparse
import fcntl
import json
import os
from pathlib import Path
import re
import shlex
import signal
import socket
import subprocess
import sys
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
STATE = ROOT / '.vscode' / '.pintos-debug'
CONTROL = STATE / 'control.sock'
PORT = 1234
stopping = threading.Event()
ready = threading.Event()
child = None


def stop():
    try:
        with socket.socket(socket.AF_UNIX) as sock:
            sock.settimeout(3)
            sock.connect(str(CONTROL))
            sock.sendall(b'STOP')
            sock.recv(16)
    except (FileNotFoundError, ConnectionRefusedError):
        pass


def check():
    with socket.socket(socket.AF_UNIX) as sock:
        sock.settimeout(3)
        sock.connect(str(CONTROL))
        sock.sendall(b'READY?')
        if sock.recv(16) != b'READY':
            raise RuntimeError('커널 디버깅 준비가 끝나지 않았습니다. 종료 후 다시 시작하세요.')
    print('Pintos 빌드 및 GDB 연결 준비 확인 완료')


def terminate(proc):
    if proc is None or proc.poll() is not None:
        return
    # Each child gets its own session; never touch unrelated QEMU/GDB sessions.
    os.killpg(proc.pid, signal.SIGINT)
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        os.killpg(proc.pid, signal.SIGKILL)
        proc.wait()


def run(cmd, cwd, capture=False):
    global child
    if stopping.is_set():
        raise InterruptedError('디버깅 준비가 취소되었습니다.')
    child = subprocess.Popen(cmd, cwd=cwd, start_new_session=True,
                             stdout=subprocess.PIPE if capture else None,
                             text=True)
    output, _ = child.communicate()
    if stopping.is_set():
        raise InterruptedError('디버깅 준비가 취소되었습니다.')
    if child.returncode:
        raise RuntimeError(f'명령이 실패했습니다 (코드 {child.returncode}): {shlex.join(cmd)}')
    return output


def command_for(test, build):
    if not re.fullmatch(r'tests/(?:userprog|vm|threads|filesys)/[a-zA-Z0-9_/-]+', test):
        raise ValueError('테스트 경로는 tests/vm/lazy-anon 같은 형식이어야 합니다.')
    # Force only the output recipe to be shown, even if a cached result exists.
    text = run(['make', '--no-print-directory', '-n', '-W', 'os.dsk', test + '.output'], build, True)
    commands = [line for line in text.splitlines() if line.startswith('pintos ')]
    if len(commands) != 1:
        raise ValueError(f'테스트 실행 명령을 찾을 수 없습니다: {test}')
    args = shlex.split(commands[0])
    args = args[:args.index('<')]
    args[0] = str(ROOT / 'pintos/utils/pintos')
    # Pausing at breakpoints must not consume the test's wall-clock timeout.
    i = args.index('-T')
    args[i + 1] = '0'
    args.insert(1, '--gdb')
    files = [args[i + 1].split(':', 1)[0] for i, arg in enumerate(args) if arg == '-p']
    if files:
        run(['make', '--no-print-directory', '-j4', *files], build)
    return [sys.executable, *args]


def serve(test, project):
    global child
    if sys.platform != 'linux':
        raise RuntimeError('VS Code에서 Dev Containers: Reopen in Container를 먼저 선택하세요.')
    STATE.mkdir(parents=True, exist_ok=True)
    stop()
    lock = (STATE / 'session.lock').open('w')
    deadline = time.monotonic() + 12
    while True:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            break
        except BlockingIOError:
            if time.monotonic() > deadline:
                raise RuntimeError('이전 디버깅 정리가 끝나지 않았습니다. 중지 후 다시 시작하세요.')
            time.sleep(.1)
    CONTROL.unlink(missing_ok=True)
    server = socket.socket(socket.AF_UNIX)
    server.bind(str(CONTROL))
    server.listen(1)
    server.settimeout(.2)

    def control():
        while not stopping.is_set():
            try:
                conn, _ = server.accept()
                with conn:
                    conn.settimeout(1)
                    request = conn.recv(16)
                    if request == b'READY?':
                        alive = child is not None and child.poll() is None
                        conn.sendall(b'READY' if ready.is_set() and alive and not stopping.is_set() else b'WAIT')
                    elif request == b'STOP':
                        stopping.set()
                        conn.sendall(b'OK')
            except socket.timeout:
                continue
            except OSError:
                return

    threading.Thread(target=control, daemon=True).start()
    signal.signal(signal.SIGTERM, lambda *_: stopping.set())
    signal.signal(signal.SIGINT, lambda *_: stopping.set())

    def cancel_child():
        stopping.wait()
        terminate(child)
    threading.Thread(target=cancel_child, daemon=True).start()

    print('PINTOS_DEBUG_STARTING', flush=True)
    try:
        with socket.socket() as probe:
            try:
                probe.bind(('0.0.0.0', PORT))
            except OSError:
                raise RuntimeError('1234 포트가 사용 중입니다. 기존 수동 GDB/QEMU 세션을 먼저 종료하세요.')
        project_dir = ROOT / 'pintos' / project
        run(['make', '--no-print-directory', '-j4'], project_dir)
        build = project_dir / 'build'
        cmd = command_for(test, build)
        (STATE / 'last-run.json').write_text(json.dumps({'test': test, 'project': project, 'command': cmd}, ensure_ascii=False, indent=2))
        print('\n실행 테스트: ' + test, flush=True)
        print(shlex.join(cmd), flush=True)
        child = subprocess.Popen(cmd, cwd=build, start_new_session=True)
        deadline = time.monotonic() + 15
        while child.poll() is None and not stopping.is_set():
            # A TCP probe would steal the GDB connection; inspect LISTEN instead.
            listeners = Path('/proc/net/tcp').read_text() + Path('/proc/net/tcp6').read_text()
            if any(len(parts := line.split()) > 3 and parts[1].endswith(':04D2') and parts[3] == '0A' for line in listeners.splitlines()):
                bootstrap = subprocess.run(['gdb', '-q', '-nx', '-batch', 'kernel.o',
                    '-ex', 'target remote 127.0.0.1:1234', '-ex', 'thbreak main',
                    '-ex', 'continue', '-ex', 'disconnect'], cwd=build,
                    stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                    text=True, timeout=30)
                print(bootstrap.stdout, flush=True)
                if bootstrap.returncode or 'Temporary breakpoint 1, main' not in bootstrap.stdout:
                    raise RuntimeError('커널 main 진입에 실패했습니다. 위 GDB 로그를 확인하세요.')
                ready.set()
                print('PINTOS_DEBUG_READY', flush=True)
                break
            if time.monotonic() > deadline:
                raise RuntimeError('QEMU가 GDB 연결을 준비하지 못했습니다.')
            time.sleep(.1)
        else:
            if not stopping.is_set():
                raise RuntimeError('QEMU가 GDB 연결 전에 종료되었습니다.')
        while child.poll() is None and not stopping.wait(.15):
            pass
    finally:
        ready.clear()
        terminate(child)
        server.close()
        CONTROL.unlink(missing_ok=True)
        print('\nPINTOS_DEBUG_STOPPED', flush=True)
        lock.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('action', choices=['serve', 'stop', 'check'])
    parser.add_argument('--test')
    parser.add_argument('--project', choices=['vm', 'userprog', 'threads'], default='vm')
    args = parser.parse_args()
    try:
        if args.action == 'stop':
            stop()
        elif args.action == 'check':
            check()
        else:
            last = STATE / 'last-run.json'
            test = args.test or (json.loads(last.read_text())['test'] if last.exists() else 'tests/vm/lazy-anon')
            serve(test, args.project)
    except (RuntimeError, ValueError, InterruptedError, OSError, subprocess.TimeoutExpired) as exc:
        print(f'Pintos 디버깅: {exc}', file=sys.stderr)
        sys.exit(1)

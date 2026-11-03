"""Native Multisim 10 automation. All editor work runs in an owned process."""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import shutil
import subprocess
import sys
import tempfile
import time
import os
import threading
import functools
import uuid
from pathlib import Path
from spice_raw import read_raw, SimulationResult
from probe_samples import ProbeSamples

ROOT = Path(__file__).resolve().parent
INSTALL = Path(os.environ.get("MULTISIM_HOME", r"D:\Multisim10\Circuit Design Suite 10.0")).resolve()

# Resource and feature identifiers from each module's native placement call.
# The server name is case-sensitive, even though the DLL filename is not.
INSTRUMENTS = {
    'scope': ('scope', 'OScilloscopeManager_Snapin', 6, 0x2f0000),
    'mulmeter': ('Mulmeter', 'MultimeterManager_Snapin', 5, 0x250000),
    'fgen': ('fgen', 'FunctionGeneratorManager_Snapin', 5, 0x1e0000),
}


class MultisimError(RuntimeError):
    pass


def _serialized(method):
    @functools.wraps(method)
    def call(self,*args,**kwargs):
        with self._lock:
            return method(self,*args,**kwargs)
    return call


def verify_installation():
    manifest = json.loads((ROOT / "native-build.json").read_text(encoding="utf-8"))
    for name, digest in manifest["sha256"].items():
        path = INSTALL / name
        if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != digest:
            raise MultisimError(f"Unsupported native binary: {path}")
    for name,digest in manifest.get('runtime_sha256',{}).items():
        path = Path(name)
        if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest()!=digest:
            raise MultisimError(f'Unsupported native runtime: {path}')
    if not (ROOT / "native_host.exe").is_file() or not (ROOT / "native_bridge.dll").is_file():
        raise MultisimError("Build the x86 native bridge with build_native.cmd first.")


def _text(value):
    value = str(value)
    if any(c in value for c in "\t\r\n\x00") or len(value) > 32000:
        raise ValueError("Protocol fields cannot contain tabs, newlines, or NUL.")
    return value


def _number(value):
    value = float(value)
    if not math.isfinite(value):
        raise ValueError("Expected a finite numeric value in SI units.")
    return value


class Session:
    """A private editable document; source files are copied before opening.

    Use as a context manager. Native calls are serialized by the helper's named
    mutex. A failing batch may have edited memory; only save after success.
    """

    def __init__(self, source=None, *, timeout=45, history_limit=0):
        if type(history_limit) is not int or not 0 <= history_limit <= 256:
            raise ValueError("history_limit must be an integer from 0 to 256.")
        self._lock = threading.RLock()
        self._faulted = False
        self._tainted = False
        self._history_limit = history_limit
        self._undo = []
        self._redo = []
        verify_installation()
        self.timeout = timeout
        self._temporary = tempfile.TemporaryDirectory(prefix="multisim-api-")
        self.directory = Path(self._temporary.name)
        self.process = None
        self._restored_path = None
        # The native instrument archive keys its records by the sheet name.
        self.path = self.directory / (Path(source).name if source else "document.ms10")
        source = Path(source).resolve() if source else ROOT / "results" / "blank-template.ms10"
        if not source.is_file():
            self._temporary.cleanup()
            raise FileNotFoundError(source)
        shutil.copy2(source, self.path)
        try:
            self._start_editor(self.path)
        except BaseException:
            self.close()
            raise

    def _start_editor(self, path):
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 0
        self.process = subprocess.Popen(
            [str(INSTALL / "multisim.exe"), str(path)],
            cwd=self.directory, startupinfo=startup,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        deadline = time.monotonic() + self.timeout
        last_error = None
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise MultisimError(f"Multisim exited during startup: {self.process.returncode}")
            try:
                state = self._host("inspect")
                if state.get("editor_ready"):
                    return
            except MultisimError as error:
                if self._faulted:
                    raise
                last_error = error
            time.sleep(0.5)
        raise MultisimError(f"Editor initialization timed out. {last_error or ''}")

    @property
    def pid(self):
        return self.process.pid if self.process else None

    def _host(self, action, request=None):
        if self._faulted:
            raise MultisimError('A native fault invalidated this session; close it and create a new Session.')
        if self.process is None or self.process.poll() is not None:
            raise MultisimError("This session is closed or its owned process exited.")
        command = [str(ROOT / "native_host.exe"), str(self.process.pid), action]
        if request is not None:
            command.append(str(request))
        result = subprocess.run(command, capture_output=True, timeout=65,
                                encoding="utf-8", errors="replace")
        reports = []
        for line in result.stdout.splitlines():
            try:
                reports.append(json.loads(line))
            except json.JSONDecodeError:
                continue
        if result.returncode or not reports:
            if result.returncode == 20:
                self._faulted = True
            detail = result.stderr.strip() or (json.dumps(reports[-1], ensure_ascii=False) if reports else result.stdout.strip())
            raise MultisimError(f"Native {action} failed ({result.returncode}): {detail}")
        report = reports[-1]
        if report.get("pid") != self.process.pid:
            raise MultisimError("Native response belongs to a different process.")
        return report

    @_serialized
    def execute(self, operations):
        if self._tainted:
            raise MultisimError('A failed batch may have partially edited memory; inspect or close this session and start a new one.')
        rows = []
        for operation in operations:
            op = operation["op"]
            if op == "place":
                x, y = operation["position"]
                rows.append([op, operation["ref"], operation["group"], operation["family"],
                             operation["part"], int(x), int(y)])
            elif op == 'place_instrument':
                instrument = str(operation['instrument']).casefold()
                if instrument not in INSTRUMENTS:
                    raise ValueError(f'Unsupported instrument: {instrument}. Available: {", ".join(INSTRUMENTS)}.')
                x, y = operation['position']
                rows.append([op, operation['ref'], *INSTRUMENTS[instrument], int(x), int(y)])
            elif op == 'configure_generator':
                waveform = str(operation.get('waveform', 'sine')).casefold()
                modes = {'sine': 0, 'triangle': 1, 'square': 2}
                if waveform not in modes:
                    raise ValueError('Waveform must be sine, triangle, or square.')
                frequency = _number(operation['frequency'])
                amplitude = _number(operation['amplitude'])
                offset = _number(operation.get('offset', 0))
                duty = _number(operation.get('duty', 50))
                if frequency <= 0 or amplitude < 0:
                    raise ValueError('Frequency must be positive and peak amplitude must be nonnegative.')
                if not 0 < duty < 100:
                    raise ValueError('Duty must be strictly between 0 and 100 percent.')
                rows.append([op, operation['ref'], modes[waveform], repr(frequency), repr(amplitude), repr(offset), repr(duty)])
            elif op == 'configure_scope':
                scales = [operation.get(key) for key in ('seconds_per_div', 'a_volts_per_div', 'b_volts_per_div')]
                scales = [None if value is None else _number(value) for value in scales]
                if any(value is not None and not 1e-12 <= value <= 1e6 for value in scales):
                    raise ValueError('Oscilloscope scales must be between 1e-12 and 1e6.')
                trigger = []
                for key, choices in [('trigger_mode', ('none', 'single', 'normal', 'auto')),
                                     ('trigger_source', ('a', 'b', 'external')),
                                     ('trigger_edge', ('rising', 'falling'))]:
                    value = operation.get(key)
                    if value is None:
                        trigger.append('')
                        continue
                    value = str(value).casefold()
                    if value not in choices:
                        raise ValueError(f'{key} must be one of {", ".join(choices)}.')
                    trigger.append(choices.index(value))
                level = operation.get('trigger_level')
                if all(value is None for value in scales) and trigger == ['', '', ''] and level is None:
                    raise ValueError('Provide at least one oscilloscope setting.')
                rows.append([op, operation['ref'], *('' if value is None else repr(value) for value in scales),
                             *trigger, '' if level is None else repr(_number(level))])
            elif op == 'configure_multimeter':
                mode = str(operation['mode']).casefold()
                modes = {'current': 0, 'voltage': 1, 'resistance': 2, 'decibel': 3}
                if mode not in modes:
                    raise ValueError('Multimeter mode must be current, voltage, resistance, or decibel.')
                rows.append([op, operation['ref'], modes[mode]])
            elif op == 'junction':
                x,y = operation['position']
                rows.append([op,operation['ref'],int(x),int(y)])
            elif op == 'net_label':
                x,y=operation.get('position') or ('','')
                rows.append([op,operation['ref'],operation['pin'],operation.get('name') or '',
                             operation.get('caption') or '',int(operation.get('visible',True)),
                             int(x) if x!='' else '',int(y) if y!='' else '',int(operation.get('caption') is not None)])
            elif op in {"wire", "disconnect"}:
                a, b = operation["from"], operation["to"]
                rows.append([op, a[0], a[1], b[0], b[1]])
            elif op in {'split_wire','insert_probe'}:
                a,b=operation['from'],operation['to']
                x,y=operation['position']
                rows.append([op,a[0],a[1],b[0],b[1],operation['ref'],int(x),int(y)])
                if op=='insert_probe':
                    kind=int(operation.get('kind',5))
                    if kind not in {3,4,5}:
                        raise ValueError('Supported native static probe types: 3 (AC voltage), 4 (AC current), 5 (instantaneous voltage/current).')
                    rows[-1].append(kind)
            elif op == 'route':
                a,b = operation['from'],operation['to']
                waypoints = operation.get('via',[])
                if len(waypoints)>1024:
                    raise ValueError('A route supports up to 1024 interior waypoints.')
                rows.append([op,a[0],a[1],b[0],b[1],*[int(coordinate) for point in waypoints for coordinate in point]])
            elif op == "value":
                rows.append([op, operation["ref"], repr(_number(operation["value"]))])
            elif op == "parameter":
                index = int(operation["index"])
                if not 0 <= index <= 256:
                    raise ValueError("Parameter index must be between 0 and 256.")
                rows.append([op, operation["ref"], index, repr(_number(operation["value"]))])
            elif op == "place_bus":
                name = operation["name"]
                if not isinstance(name, str) or not 1 <= len(name) <= 64:
                    raise ValueError("A bus name requires 1 to 64 characters.")
                points = [tuple(point) for point in operation["points"]]
                if not 2 <= len(points) <= 256:
                    raise ValueError("A bus requires 2 to 256 points.")
                if any(len(point) != 2 or any(type(v) is not int or abs(v) > 1000000 for v in point)
                       for point in points):
                    raise ValueError("Bus points must be integer coordinate pairs within +/-1000000.")
                if any((a[0] == b[0]) == (a[1] == b[1]) for a, b in zip(points, points[1:])):
                    raise ValueError("Bus segments must be nonzero and horizontal or vertical.")
                rows.append([op, name, *(value for point in points for value in point)])
            elif op == "move":
                x, y = operation["position"]
                rows.append([op, operation["ref"], int(x), int(y)])
            elif op == "rotate":
                rows.append([op, operation["ref"], repr(_number(operation["degrees"]))])
            elif op == "mirror":
                axis = str(operation["axis"]).casefold()
                if axis not in {"horizontal", "vertical"}:
                    raise ValueError("Mirror axis must be horizontal or vertical.")
                rows.append([op, operation["ref"], axis])
            elif op == "duplicate":
                x, y = operation["position"]
                rows.append([op, operation["source"], operation["ref"], int(x), int(y)])
            elif op == "delete":
                rows.append([op, operation["ref"]])
            elif op == "catalog":
                rows.append([op, operation["keyword"], int(operation.get("limit", 100))])
            elif op == "export_netlist":
                rows.append([op, str(Path(operation["path"]).resolve()), int(operation.get("format", 0))])
            elif op == "save_as":
                rows.append([op, str(Path(operation["path"]).resolve())])
            else:
                raise ValueError(f"Unknown operation: {op}")
        if not rows:
            return {"pid": self.pid, "ok": True, "results": []}
        payload = "\n".join("\t".join(_text(x) for x in row) for row in rows) + "\n"
        track = self._history_limit and any(row[0] not in {'catalog', 'export_netlist', 'save_as'} for row in rows)
        checkpoint = self._capture_history() if track else None
        request = self.directory / "request.tsv"
        try:
            request.write_text(payload, encoding="utf-16")
            report = self._host("batch", request)
        except (MultisimError,subprocess.TimeoutExpired):
            self._tainted = True
            if checkpoint:
                self._discard_checkpoint(checkpoint)
            raise
        except OSError:
            if checkpoint:
                self._discard_checkpoint(checkpoint)
            raise
        if checkpoint:
            self._undo.append(checkpoint)
            if len(self._undo) > self._history_limit:
                self._discard_checkpoint(self._undo.pop(0))
            self._clear_history(self._redo)
        return report

    def _capture_history(self):
        checkpoint = self.directory / f'history-{uuid.uuid4().hex}' / self.path.name
        current = self.path
        try:
            try:
                return self.save(checkpoint)
            finally:
                self.path = current
                if not self._faulted and not self._tainted:
                    self.execute([dict(op='save_as', path=current)])
        except BaseException:
            self._discard_checkpoint(checkpoint)
            raise

    @staticmethod
    def _discard_checkpoint(checkpoint):
        try:
            checkpoint.unlink(missing_ok=True)
            checkpoint.parent.rmdir()
        except OSError:
            # The enclosing private session directory is also cleaned on close.
            pass

    def _clear_history(self, stack):
        for checkpoint in stack:
            self._discard_checkpoint(checkpoint)
        stack.clear()

    def _restore_history(self, source, destination):
        if self._tainted or self._faulted:
            raise MultisimError('The current editor session is invalid.')
        if not self._history_limit:
            raise MultisimError('Enable edit history with Session(history_limit=...).')
        if not source:
            raise MultisimError('No matching edit history is available.')
        current = self._capture_history()
        target = source[-1]
        try:
            state = self._open_history(target)
        except (MultisimError, OSError, subprocess.TimeoutExpired):
            try:
                self._open_history(current)
            except (MultisimError, OSError, subprocess.TimeoutExpired) as recovery_error:
                self._faulted = True
                raise MultisimError('History restore failed and the previous editor state could not be reopened.') from recovery_error
            self._discard_checkpoint(current)
            raise
        destination.append(current)
        self._discard_checkpoint(source.pop())
        return state

    def _open_history(self, checkpoint):
        working = self.directory / f'restored-{uuid.uuid4().hex}' / checkpoint.name
        working.parent.mkdir()
        shutil.copy2(checkpoint, working)
        self._stop_editor()
        self._restored_path = working
        self._faulted = self._tainted = False
        self._start_editor(working)
        self.execute([dict(op='save_as', path=self.path)])
        return self.inspect()

    @_serialized
    def undo(self):
        """Restore the document to the state before the last successful edit batch."""
        return self._restore_history(self._undo, self._redo)

    @_serialized
    def redo(self):
        """Restore the document state removed by undo."""
        return self._restore_history(self._redo, self._undo)

    def place(self, ref, group, family, part, x, y):
        return self.execute([dict(op="place", ref=ref, group=group, family=family, part=part, position=[x, y])])

    def place_instrument(self, ref, instrument, x, y):
        """Place a native scope, mulmeter, or fgen and initialize its terminals."""
        return self.execute([dict(op='place_instrument', ref=ref, instrument=instrument, position=[x, y])])

    def configure_generator(self, ref, *, frequency, amplitude, offset=0, waveform='sine', duty=50):
        """Set a native FGEN waveform in hertz, peak volts, offset volts and duty percent."""
        return self.execute([dict(op='configure_generator', ref=ref, frequency=frequency,
                                  amplitude=amplitude, offset=offset, waveform=waveform, duty=duty)])

    def configure_multimeter(self, ref, mode):
        """Select a native multimeter measurement mode."""
        return self.execute([dict(op='configure_multimeter', ref=ref, mode=mode)])

    def configure_scope(self, ref, *, seconds_per_div=None, a_volts_per_div=None, b_volts_per_div=None,
                        trigger_mode=None, trigger_source=None, trigger_edge=None, trigger_level=None):
        """Set native oscilloscope scales and trigger settings."""
        return self.execute([dict(op='configure_scope', ref=ref, seconds_per_div=seconds_per_div,
                                  a_volts_per_div=a_volts_per_div, b_volts_per_div=b_volts_per_div,
                                  trigger_mode=trigger_mode, trigger_source=trigger_source,
                                  trigger_edge=trigger_edge, trigger_level=trigger_level)])

    def _interactive_simulation(self, action):
        request = self.directory / 'interactive-simulation.tsv'
        request.write_text(f'interactive_simulation\t{action}\n', encoding='utf-16')
        return self._host('batch', request)['results'][0]['running']

    def _wait_interactive_state(self, running):
        deadline = time.monotonic() + self.timeout
        while time.monotonic() < deadline:
            if self._interactive_simulation('status') == running:
                return
            time.sleep(0.05)
        raise MultisimError(f'Interactive simulation did not {"start" if running else "stop"}.')

    def _run_interactive(self, run_seconds):
        run_seconds = _number(run_seconds)
        if not 0 < run_seconds <= 60:
            raise ValueError('run_seconds must be between 0 and 60 seconds.')
        if self._tainted or self._faulted:
            raise MultisimError('The current editor session is invalid.')

        if self._interactive_simulation('status'):
            raise MultisimError('The editor is already simulating.')
        try:
            self._interactive_simulation('start')
            self._wait_interactive_state(True)
            time.sleep(run_seconds)
        finally:
            try:
                self._interactive_simulation('stop')
                self._wait_interactive_state(False)
            except (MultisimError, OSError, subprocess.TimeoutExpired):
                self._tainted = True
                raise

    def _instrument(self, ref, kind):
        return next((item for item in self.inspect()['components']
                     if ref in (item['refdes'], item['electrical_name'])
                     and item.get('instrument', {}).get('type') == kind), None)

    @_serialized
    def measure_multimeter(self, ref, *, run_seconds=2.0):
        """Run interactive simulation and read the native meter panel after stopping."""
        target = self._instrument(ref, 'MULMETER')
        if not target:
            raise ValueError(f'No native multimeter named {ref}.')
        if sum(bool(pin['node_name']) for pin in target['pins']) != 2:
            raise ValueError(f'Both terminals of {ref} must be wired.')
        self._run_interactive(run_seconds)
        deadline = time.monotonic() + min(5, self.timeout)
        while time.monotonic() < deadline:
            meter = (self._instrument(ref, 'MULMETER') or {}).get('instrument', {}).get('meter', {})
            if meter.get('ready'):
                return meter
            time.sleep(0.05)
        raise MultisimError(f'Multimeter {ref} produced no panel reading.')

    @_serialized
    def sample_scope(self, ref, *, run_seconds=2.0, max_points=4096):
        """Run interactive simulation and return native [seconds, volts] pairs by channel."""
        if type(max_points) is not int or not 2 <= max_points <= 20000:
            raise ValueError('max_points must be an integer from 2 to 20000.')
        target = self._instrument(ref, 'SCOPE')
        if not target:
            raise ValueError(f'No native two-channel oscilloscope named {ref}.')
        signal_pins = {pin['terminal'] for pin in target['pins'] if pin['node_name']}
        expected = {channel for channel, pin in [('A', '1'), ('B', '2')] if pin in signal_pins}
        if not expected:
            raise ValueError(f'At least one signal input of {ref} must be wired.')
        request = self.directory / 'scope-read.tsv'
        request.write_text(f"scope_read\t{_text(target['electrical_name'])}\t{max_points}\n", encoding='utf-16')
        self._run_interactive(run_seconds)
        deadline = time.monotonic() + min(5, self.timeout)
        while time.monotonic() < deadline:
            channels = self._host('batch', request)['results'][0]['channels']
            if expected <= {item['channel'] for item in channels}:
                return {'channels': [item for item in channels if item['channel'] in expected]}
            time.sleep(0.05)
        raise MultisimError(f'Oscilloscope {ref} produced no complete channel data.')

    def wire(self, source, source_pin, target, target_pin):
        return self.execute([{"op": "wire", "from": [source, source_pin], "to": [target, target_pin]}])

    def junction(self, ref, x, y):
        return self.execute([dict(op='junction',ref=ref,position=[x,y])])

    def place_bus(self, name, points):
        """Place a native bus backbone through the supplied orthogonal vertices."""
        return self.execute([dict(op='place_bus', name=name, points=points)])

    def label_net(self, ref, pin, name=None, *, caption=None, visible=True, position=None):
        """Set the native electrical net name and optional displayed caption.

        Omitting name retains the electrical name (including ground '0').
        Omitting caption retains it; an empty caption clears it.
        The terminal must belong to an existing wired scalar net.
        """
        return self.execute([dict(op='net_label',ref=ref,pin=pin,name=name,caption=caption,
                                  visible=visible,position=position)])

    def split_wire(self, source, source_pin, target, target_pin, junction, x, y):
        """Insert a native junction into an existing wire; retain its vertices.

        The point is snapped to the native grid and must lie on the wire interior.
        Junction terminals use pin '0' with wire(), route(), and disconnect().
        """
        return self.execute([dict(op='split_wire',ref=junction,position=[x,y],
                                  **{'from':[source,source_pin],'to':[target,target_pin]})])

    def insert_probe(self, source, source_pin, target, target_pin, probe, x, y, *, kind=5):
        """Insert a native static probe into an existing wire, preserving both routes.

        Native kind 5 measures instantaneous voltage/current; 3 and 4 select AC
        voltage/current display measurements. Probe terminals use pin '0'.
        """
        return self.execute([dict(op='insert_probe',ref=probe,position=[x,y],kind=kind,
                                  **{'from':[source,source_pin],'to':[target,target_pin]})])

    def disconnect(self, source, source_pin, target, target_pin):
        return self.execute([{"op": "disconnect", "from": [source, source_pin], "to": [target, target_pin]}])

    def route(self, source, source_pin, target, target_pin, via):
        return self.execute([{'op':'route','from':[source,source_pin],'to':[target,target_pin],'via':via}])

    def set_value(self, ref, value):
        return self.execute([dict(op="value", ref=ref, value=value)])

    def set_parameter(self, ref, index, value):
        return self.execute([dict(op="parameter", ref=ref, index=index, value=value)])

    def move(self, ref, x, y):
        return self.execute([dict(op="move", ref=ref, position=[x, y])])

    def rotate(self, ref, degrees):
        return self.execute([dict(op="rotate", ref=ref, degrees=degrees)])

    def mirror(self, ref, axis):
        """Flip a native symbol horizontally or vertically."""
        return self.execute([dict(op="mirror", ref=ref, axis=axis)])

    def duplicate(self, source, ref, x, y):
        """Copy a scalar component to a new reference and position."""
        return self.execute([dict(op="duplicate", source=source, ref=ref, position=[x, y])])

    def delete(self, ref):
        return self.execute([dict(op="delete", ref=ref)])

    def catalog(self, keyword, limit=100):
        return self.execute([dict(op="catalog", keyword=keyword, limit=limit)])["results"][0]["matches"]

    def export_netlist(self, path, format=0):
        path = Path(path).resolve()
        path.parent.mkdir(parents=True, exist_ok=True)
        self.execute([dict(op="export_netlist", path=path, format=format)])
        return path

    @_serialized
    def solve(self, analysis='op', *, duration=0.001, start_frequency=1,
              stop_frequency=1e6, points_per_decade=30, output_dir=None, overwrite=False):
        """Solve the native exported schematic with Multisim's own SPICE engine.

        Re-sourcing the native netlist initializes standalone analyses correctly;
        issuing OP against the live interactive engine can retain callback state.
        """
        if analysis not in {'op','transient','ac'}:
            raise ValueError('Analysis must be op, transient, or ac.')
        duration, start_frequency, stop_frequency = map(_number, [duration,start_frequency,stop_frequency])
        points_per_decade = int(points_per_decade)
        if duration <= 0 or start_frequency <= 0 or stop_frequency < start_frequency or not 1 <= points_per_decade <= 10000:
            raise ValueError('Invalid analysis duration or frequency range.')
        destination = Path(output_dir).resolve() if output_dir else self.directory / ('solve-' + str(time.time_ns()))
        if destination.exists() and any(destination.iterdir()) and not overwrite:
            raise FileExistsError(destination)
        destination.mkdir(parents=True, exist_ok=True)
        design = self.directory / 'analysis.ms10'
        netlist = self.directory / 'analysis.cir'
        previous = self.path
        try:
            self.save(design, overwrite=True)
            self.export_netlist(netlist)
            state = self.inspect()
            saved_design = destination/'design.ms10'
            shutil.copy2(design,saved_design)
            powershell = Path(os.environ.get('WINDIR', r'C:\Windows')) / 'SysWOW64' / 'WindowsPowerShell' / 'v1.0' / 'powershell.exe'
            command = [str(powershell), '-NoProfile', '-NonInteractive', '-ExecutionPolicy','Bypass',
                       '-File',str(ROOT/'simulate_design.ps1'), '-InputFile',str(saved_design),
                       '-NetlistFile',str(netlist), '-OutputDirectory',str(destination),
                       '-Analysis',analysis, '-Duration',str(duration),
                       '-StartFrequency',str(start_frequency), '-StopFrequency',str(stop_frequency),
                       '-PointsPerDecade',str(points_per_decade)]
            completed = subprocess.run(command,capture_output=True,timeout=self.timeout*2+40)
            status_path = destination/'simulation.json'
            status = json.loads(status_path.read_text(encoding='utf-8-sig')) if status_path.is_file() else {}
            if completed.returncode or not status.get('completed'):
                raise MultisimError(f"Native solver failed: {status.get('error','see solver logs')} ({destination})")
            result = SimulationResult(read_raw(destination/(analysis+'.raw')),state,destination)
            (destination/'result.json').write_text(json.dumps(result.raw,indent=2),encoding='utf-8')
            (destination/'schematic.json').write_text(json.dumps(state,indent=2),encoding='utf-8')
            return result
        finally:
            if self.process and self.process.poll() is None and not self._tainted and not self._faulted:
                self.execute([dict(op='save_as',path=previous)])
                self.path = previous

    @_serialized
    def sample_probes(self, *, sample_rate=10000, points=100, duration=None,
                      outputs=None, output_dir=None, overwrite=False):
        """Run native COM simulation and retrieve one uniformly sampled probe block.

        Output names are the native EnumOutputs names (for example V(P1)).
        EnumOutputs is called before SetOutputRequest/RunSimulation only: this
        version rebuilds circuit state when enumerating outputs after a run.
        """
        sample_rate=_number(sample_rate)
        points=int(points)
        if sample_rate<=0 or not 1<=points<=1000000:
            raise ValueError('Positive sample_rate and 1..1000000 points are required.')
        duration=_number(duration) if duration is not None else 2*points/sample_rate
        if duration<=0 or duration<(points-1)/sample_rate:
            raise ValueError('Simulation duration is too short for the requested sample block.')
        if outputs is not None:
            if isinstance(outputs,str): raise ValueError('outputs must be a sequence of native channel names.')
            outputs=[_text(output) for output in outputs]
            if not outputs or len({output.casefold() for output in outputs})!=len(outputs):
                raise ValueError('outputs must contain unique native channel names.')
        destination=Path(output_dir).resolve() if output_dir else self.directory/('samples-'+str(time.time_ns()))
        if destination.exists() and any(destination.iterdir()) and not overwrite:
            raise FileExistsError(destination)
        destination.mkdir(parents=True,exist_ok=True)
        previous=self.path
        run_id=uuid.uuid4().hex
        try:
            # Keep the saved basename through the editor and COM copies.
            self.save(self.directory/'sample-snapshot'/'design.ms10',overwrite=True)
            design=destination/'design.ms10'
            shutil.copy2(self.path,design)
            request=destination/'request.json'
            request.write_text(json.dumps(dict(run_id=run_id,source=str(design),output_dir=str(destination),
                                              sample_rate=sample_rate,points=points,duration=duration,
                                              timeout=self.timeout,outputs=outputs)),encoding='utf-8')
            powershell=Path(os.environ.get('WINDIR',r'C:\Windows'))/'SysWOW64'/'WindowsPowerShell'/'v1.0'/'powershell.exe'
            command=[str(powershell),'-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass',
                     '-File',str(ROOT/'sample_probes.ps1'),'-RequestFile',str(request)]
            worker=subprocess.Popen(command,stdout=subprocess.PIPE,stderr=subprocess.PIPE,
                                    creationflags=subprocess.CREATE_NO_WINDOW)
            try:
                stdout,stderr=worker.communicate(timeout=self.timeout*2+40)
            except subprocess.TimeoutExpired:
                if worker.poll() is None:
                    subprocess.run(['taskkill.exe','/PID',str(worker.pid),'/T','/F'],
                                   capture_output=True,timeout=15)
                worker.communicate(timeout=10)
                raise MultisimError('Native probe worker timed out; its owned process tree was terminated.')
            status_path=destination/'probe-sampling.json'
            report=json.loads(status_path.read_text(encoding='utf-8-sig')) if status_path.is_file() else {}
            if report.get('run_id')!=run_id:
                raise MultisimError('Native probe worker did not produce a matching result report.')
            native_log=Path(report['scratch'])/'native.log'
            try:
                shutil.copyfile(native_log,destination/'native.log')
            except FileNotFoundError:
                pass
            except OSError as error:
                report['native_log_copy_error']=str(error)
            report['build_sha256']=hashlib.sha256((ROOT/'native_bridge.dll').read_bytes()).hexdigest()
            report['worker_returncode']=worker.returncode
            report['worker_stdout']=stdout.decode(errors='replace')
            report['worker_stderr']=stderr.decode(errors='replace')
            (destination/'result.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
            if worker.returncode or not report.get('completed'):
                raise MultisimError(f"Native probe sampling failed: {report.get('error','see logs')} ({destination})")
            return ProbeSamples(report,destination)
        finally:
            if self.process and self.process.poll() is None and not self._tainted and not self._faulted:
                self.execute([dict(op='save_as',path=previous)])
                self.path=previous

    @_serialized
    def inspect(self, *, raw=False):
        state = self._host("inspect")
        if raw:
            return state
        objects = [x for x in state["objects"] if x.get("label") == "model_object"]
        return {"pid": self.pid, "editor_ready": state["editor_ready"],
                "components": [x for x in objects if "pins" in x],
                "wires": [x for x in objects if x.get("class") == "CIITLinkComp"],
                "buses": [x for x in objects if x.get("class") == "CIITBusCompV8"],
                "junctions": [x for x in objects if x.get('class')=='CODPinComp'],
                "net_labels": [x for x in objects if x.get('class')=='CODNodeTextComp'],
                "probes": [x for x in objects if x.get('class')=='CIITProbeComponent'],
                "other_objects": [x for x in objects if "pins" not in x and x.get("class") not in {"CIITLinkComp","CIITBusCompV8","CODPinComp","CODNodeTextComp","CIITProbeComponent"}]}

    @_serialized
    def save(self, path=None, *, overwrite=False):
        if self._tainted:
            raise MultisimError('Saving is disabled after a failed batch; discard this session.')
        target = Path(path).resolve() if path else self.path
        if target != self.path:
            if target.exists() and not overwrite:
                raise FileExistsError(target)
            target.parent.mkdir(parents=True, exist_ok=True)
            self.execute([dict(op="save_as", path=target)])
        before = target.stat().st_mtime_ns if target.exists() else None
        self._host("save")
        if not target.is_file() or target.stat().st_size < 64:
            raise MultisimError(f"Native save did not produce a valid file: {target}")
        if before is not None and target.stat().st_mtime_ns == before:
            raise MultisimError(f"Native save did not update the target: {target}")
        self.path = target
        return target

    @_serialized
    def new(self):
        self._host("new")
        deadline = time.monotonic() + self.timeout
        while time.monotonic() < deadline:
            state = self.inspect()
            if state["editor_ready"] and not state["components"] and not state["wires"] and not state["buses"]:
                self._clear_history(self._undo)
                self._clear_history(self._redo)
                return state
            time.sleep(0.5)
        raise MultisimError("Native new document did not become ready.")

    def _stop_editor(self):
        if self.process is not None and self.process.poll() is None:
            self.process.kill()
            self.process.wait(timeout=10)
        self.process = None
        if self._restored_path:
            self._discard_checkpoint(self._restored_path)
            self._restored_path = None

    @_serialized
    def close(self):
        self._stop_editor()
        self._temporary.cleanup()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    inspect_parser = commands.add_parser("inspect")
    inspect_parser.add_argument("file")
    run_parser = commands.add_parser("run")
    run_parser.add_argument("recipe")
    run_parser.add_argument("--output", required=True)
    run_parser.add_argument("--overwrite", action="store_true")
    solve_parser = commands.add_parser('solve')
    solve_parser.add_argument('file')
    solve_parser.add_argument('--analysis',choices=['op','transient','ac'],default='op')
    solve_parser.add_argument('--output-dir',required=True)
    solve_parser.add_argument('--duration',type=float,default=0.001)
    solve_parser.add_argument('--start-frequency',type=float,default=1)
    solve_parser.add_argument('--stop-frequency',type=float,default=1e6)
    solve_parser.add_argument('--points-per-decade',type=int,default=30)
    solve_parser.add_argument('--overwrite',action='store_true')
    sample_parser=commands.add_parser('sample-probes')
    sample_parser.add_argument('file')
    sample_parser.add_argument('--output-dir',required=True)
    sample_parser.add_argument('--sample-rate',type=float,default=10000)
    sample_parser.add_argument('--points',type=int,default=100)
    sample_parser.add_argument('--duration',type=float)
    sample_parser.add_argument('--output',action='append',dest='outputs')
    sample_parser.add_argument('--overwrite',action='store_true')
    catalog_parser = commands.add_parser('catalog')
    catalog_parser.add_argument('keyword')
    catalog_parser.add_argument('--limit',type=int,default=100)
    commands.add_parser('instruments', help='List installed instrument symbols and supported placement types')
    meter_parser = commands.add_parser('measure-multimeter', help='Run native interactive simulation and read a multimeter')
    meter_parser.add_argument('file')
    meter_parser.add_argument('ref')
    meter_parser.add_argument('--run-seconds', type=float, default=2.0)
    scope_parser = commands.add_parser('sample-scope', help='Read native oscilloscope time and voltage points')
    scope_parser.add_argument('file')
    scope_parser.add_argument('ref')
    scope_parser.add_argument('--run-seconds', type=float, default=2.0)
    scope_parser.add_argument('--max-points', type=int, default=4096)
    args = parser.parse_args()
    if args.command == "inspect":
        with Session(args.file) as session:
            print(json.dumps(session.inspect(), ensure_ascii=False, indent=2))
    elif args.command == 'solve':
        with Session(args.file) as session:
            result = session.solve(args.analysis,duration=args.duration,start_frequency=args.start_frequency,
                                   stop_frequency=args.stop_frequency,points_per_decade=args.points_per_decade,
                                   output_dir=args.output_dir,overwrite=args.overwrite)
            print(json.dumps(result.summary(),ensure_ascii=False,indent=2))
    elif args.command == 'catalog':
        with Session() as session:
            print(json.dumps(session.catalog(args.keyword,args.limit),ensure_ascii=False,indent=2))
    elif args.command == 'instruments':
        entries = json.loads((ROOT / 'instrument-catalog.json').read_text(encoding='utf-8'))
        for entry in entries:
            entry['placement_supported'] = entry['module'] in INSTRUMENTS
        print(json.dumps(entries, ensure_ascii=False, indent=2))
    elif args.command == 'measure-multimeter':
        with Session(args.file) as session:
            print(json.dumps(session.measure_multimeter(args.ref, run_seconds=args.run_seconds),
                             ensure_ascii=False, indent=2))
    elif args.command == 'sample-scope':
        with Session(args.file) as session:
            print(json.dumps(session.sample_scope(args.ref, run_seconds=args.run_seconds,
                                                  max_points=args.max_points), ensure_ascii=False, indent=2))
    elif args.command == 'sample-probes':
        with Session(args.file) as session:
            result=session.sample_probes(sample_rate=args.sample_rate,points=args.points,duration=args.duration,
                                         outputs=args.outputs,output_dir=args.output_dir,overwrite=args.overwrite)
            print(json.dumps(result.summary(),ensure_ascii=False,indent=2))
    else:
        recipe = json.loads(Path(args.recipe).read_text(encoding="utf-8-sig"))
        with Session(recipe.get("source")) as session:
            report = session.execute(recipe["operations"])
            target = session.save(args.output, overwrite=args.overwrite)
            print(json.dumps({"output": str(target), "operations": report}, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    try:
        main()
    except (MultisimError, ValueError, OSError, subprocess.TimeoutExpired) as error:
        print(json.dumps({"ok": False, "error": str(error)}, ensure_ascii=False), file=sys.stderr)
        sys.exit(1)

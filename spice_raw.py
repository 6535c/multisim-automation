"""Read the ASCII raw files produced by Multisim 10's SPICE engine."""
from pathlib import Path
import math


def read_raw(path):
    lines = [line for line in Path(path).read_text(encoding='ascii').splitlines() if line.strip()]
    header = {}
    cursor = 0
    while cursor < len(lines) and lines[cursor].strip() != 'Variables:':
        key, separator, value = lines[cursor].partition(':')
        if separator:
            header[key.strip()] = value.strip()
        cursor += 1
    count = int(header['No. Variables'])
    points = int(header['No. Points'])
    if not 1 <= count <= 100000 or not 1 <= points <= 10000000:
        raise ValueError('Invalid raw file dimensions.')
    cursor += 1
    variables = []
    for index in range(count):
        ordinal, name, unit = lines[cursor].split(maxsplit=2)
        if int(ordinal) != index:
            raise ValueError('Raw variable index mismatch.')
        unit, _, metadata = unit.partition(' ')
        variables.append({'name':name, 'unit':unit, 'metadata':metadata, 'values':[]})
        cursor += 1
    if lines[cursor].strip() != 'Values:':
        raise ValueError('Expected ASCII raw values.')
    cursor += 1
    is_complex = 'complex' in header.get('Flags','').split()
    for index in range(points):
        for variable in range(count):
            tokens = lines[cursor].split()
            cursor += 1
            if variable == 0:
                if len(tokens) != 2 or int(tokens[0]) != index:
                    raise ValueError('Raw sample index mismatch.')
                value = tokens[1]
            else:
                if len(tokens) != 1:
                    raise ValueError('Malformed raw sample.')
                value = tokens[0]
            numeric = [float(x) for x in value.split(',')]
            if len(numeric) != (2 if is_complex else 1) or not all(map(math.isfinite,numeric)):
                raise ValueError('Invalid or non-finite solver result.')
            variables[variable]['values'].append(numeric if is_complex else numeric[0])
    if any(line.strip() for line in lines[cursor:]):
        raise ValueError('Unexpected trailing data or multiple plots.')
    return {'title':header.get('Title',''), 'plot':header.get('Plotname',''),
            'points':points, 'complex':is_complex, 'variables':variables}


class SimulationResult:
    def __init__(self, raw, state, directory):
        self.raw, self.state, self.directory = raw, state, Path(directory)
        self._variables = {v['name'].lstrip('$').casefold():v['values'] for v in raw['variables']}

    def voltage(self, ref, pin):
        component = next(c for c in self.state['components'] if ref in (c['refdes'], c['electrical_name']))
        terminal = next(p for p in component['pins'] if p.get('terminal', p['number'])==str(pin))
        node = terminal['node_name'].casefold()
        if not node:
            raise ValueError(f'Unconnected pin: {ref}.{pin}')
        if node == '0':
            zero = [0.0,0.0] if self.raw['complex'] else 0.0
            return [zero for _ in range(self.raw['points'])]
        for candidate in [node, f'v({node})']:
            if candidate in self._variables:
                return self._variables[candidate]
        raise KeyError(f'No voltage vector for node {node}')

    def current(self, ref):
        for candidate in [f'v{ref}#branch',f'l{ref}#branch',f'i({ref})']:
            if candidate.casefold() in self._variables:
                return self._variables[candidate.casefold()]
        raise KeyError(f'No branch-current vector for {ref}')

    def summary(self):
        return {'plot':self.raw['plot'], 'points':self.raw['points'],
                'complex':self.raw['complex'], 'directory':str(self.directory),
                'last':{v['name']:v['values'][-1] for v in self.raw['variables']}}

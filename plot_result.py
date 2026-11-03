"""Turn a native solve result.json into a standalone SVG figure."""
import argparse
import html
import json
import math
from pathlib import Path


def plot_result(result, trace, output, *, reference=None):
    variables = {item['name'].casefold(): item for item in result['variables']}
    vectors = {name: item['values'] for name, item in variables.items()}
    ac = result['complex']
    axis = 'frequency' if ac else 'time'
    if axis not in vectors:
        raise ValueError('This figure requires an AC or transient result.')
    if reference and not ac:
        raise ValueError('Reference vectors are available for AC gain figures.')
    if ac:
        x = [math.log10(value[0]) for value in vectors[axis]]
        values = [complex(*value) for value in vectors[trace.casefold()]]
        if reference:
            denominator = [complex(*value) for value in vectors[reference.casefold()]]
            values = [a / b for a, b in zip(values, denominator, strict=True)]
        y = [20 * math.log10(max(abs(value), 1e-15)) for value in values]
        xlabel, ylabel = 'Frequency (Hz)', 'Gain (dB)' if reference else 'Magnitude (dB re 1 unit)'
    else:
        x, y = vectors[axis], vectors[trace.casefold()]
        unit = variables[trace.casefold()]['unit']
        ylabel = {'voltage': 'Voltage (V)', 'current': 'Current (A)'}.get(unit, 'Value (native unit)')
        xlabel = 'Time (s)'
    if not x or len(x) != len(y) or any(not math.isfinite(v) for v in (*x, *y)):
        raise ValueError('Expected finite paired samples.')
    left, top, width, height = 105, 70, 710, 365
    xmin, xmax, ymin, ymax = min(x), max(x), min(y), max(y)
    if xmin == xmax:
        xmax = xmin + 1
    margin = (ymax - ymin) * 0.06 or max(abs(ymax), 1) * 0.05
    ymin, ymax = ymin - margin, ymax + margin
    coordinates = ' '.join(f'{left+width*(a-xmin)/(xmax-xmin):.2f},{top+height*(ymax-b)/(ymax-ymin):.2f}'
                           for a, b in zip(x, y))
    caption = html.escape(trace + (f' / {reference}' if reference else ''))
    lines = ['<svg xmlns="http://www.w3.org/2000/svg" width="860" height="510" viewBox="0 0 860 510">',
             '<rect width="860" height="510" fill="white"/>',
             '<g font-family="Segoe UI, sans-serif" fill="#1f2937" font-size="14">',
             f'<text x="105" y="39" font-size="23">{caption}</text>',
             f'<text x="460" y="493" text-anchor="middle">{xlabel}</text>',
             f'<text x="24" y="250" text-anchor="middle" transform="rotate(-90 24 250)">{ylabel}</text>']
    for i in range(6):
        px = left + width * i / 5
        py = top + height * i / 5
        xv = xmin + (xmax - xmin) * i / 5
        yv = ymax - (ymax - ymin) * i / 5
        lines.extend([f'<path d="M {px} {top} v {height} M {left} {py} h {width}" stroke="#d7dee6" fill="none"/>',
                      f'<text x="{px}" y="459" text-anchor="middle">{(10**xv if ac else xv):.4g}</text>',
                      f'<text x="95" y="{py+5}" text-anchor="end">{yv:.4g}</text>'])
    lines.extend([f'<polyline points="{coordinates}" fill="none" stroke="#145da0" stroke-width="2"/>',
                  '</g></svg>'])
    output.write_text('\n'.join(lines), encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('result', type=Path)
    parser.add_argument('trace', help='Exact vector name from result.json, e.g. V(OUT)')
    parser.add_argument('output', type=Path)
    parser.add_argument('--reference', help='Optional AC input vector for a gain ratio')
    args = parser.parse_args()
    plot_result(json.loads(args.result.read_text(encoding='utf-8')), args.trace, args.output,
                reference=args.reference)

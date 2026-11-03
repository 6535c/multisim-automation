"""Numeric checks against independent DC and AC circuit equations."""
import cmath
import json
import math
import hashlib
from multisim import ROOT, Session

report = {'build_sha256':hashlib.sha256((ROOT/'native_bridge.dll').read_bytes()).hexdigest()}
with Session() as s:
    s.execute(json.loads((ROOT/'examples'/'voltage_divider.json').read_text())['operations'])
    s.save(ROOT/'results'/'api-voltage-divider.ms10',overwrite=True)
with Session(ROOT/'results'/'api-voltage-divider.ms10') as s:
    for analysis in ['op','transient']:
        result = s.solve(analysis,output_dir=ROOT/'results'/('api-divider-'+analysis),overwrite=True)
        voltage = result.voltage('R1','2')
        current = result.current('V1')
        assert max(abs(x-2.5) for x in voltage) < 1e-9, voltage[-1]
        assert max(abs(x+0.0025) for x in current) < 1e-12, current[-1]
        report[analysis] = result.summary()
with Session() as s:
    s.execute(json.loads((ROOT/'examples'/'rc_lowpass.json').read_text())['operations'])
    s.save(ROOT/'results'/'api-rc-lowpass.ms10',overwrite=True)
with Session(ROOT/'results'/'api-rc-lowpass.ms10') as s:
    result = s.solve('ac',start_frequency=10,stop_frequency=10000,points_per_decade=10,
                     output_dir=ROOT/'results'/'api-rc-ac',overwrite=True)
    freq = next(v['values'] for v in result.raw['variables'] if v['unit']=='frequency')
    actual = [complex(*x) for x in result.voltage('C1','1')]
    expected = [1/(1+2j*math.pi*x[0]*1000*1e-6) for x in freq]
    error = max(abs(a-b) for a,b in zip(actual,expected))
    assert len(actual)==31 and error < 1e-8, (len(actual),error)
    report['ac'] = dict(result.summary(),max_complex_error=error)
report['passed'] = True
(ROOT/'results'/'solve-verification.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
print(json.dumps(report,indent=2))

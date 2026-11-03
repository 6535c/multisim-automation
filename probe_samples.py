"""Native COM probe sample blocks returned by Multisim 10."""
from __future__ import annotations


class ProbeSamples:
    def __init__(self, report, directory):
        self.report = report
        self.directory = directory

    @property
    def channels(self):
        return tuple(channel['name'] for channel in self.report['channels'])

    def _channel(self, name):
        for channel in self.report['channels']:
            if channel['name'].casefold() == str(name).casefold():
                return channel
        raise KeyError(f'Unknown native probe channel: {name}; available: {self.channels}')

    def time(self, channel):
        return list(self._channel(channel)['time'])

    def values(self, channel):
        return list(self._channel(channel)['values'])

    def summary(self):
        return {'directory': str(self.directory), 'channels': [
            {'name': c['name'], 'points': len(c['values']),
             'first_time': c['time'][0], 'last_time': c['time'][-1],
             'first': c['values'][0], 'last': c['values'][-1]}
            for c in self.report['channels']]}

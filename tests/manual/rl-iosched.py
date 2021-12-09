#!/bin/env python3

import yaml
import time
import subprocess
import argparse
import shutil

parser = argparse.ArgumentParser(description='IO scheduler tester')
parser.add_argument('--directory', help='Directory to run on', default='/mnt')
parser.add_argument('--seastar-build-dir', help='Path to seastar build directory', default='./build/dev/', dest='bdir')
parser.add_argument('--duration', help='One run duration', default=60)
args = parser.parse_args()


class iotune:
    def __init__(self, args):
        self._iotune = args.bdir + '/apps/iotune/iotune'
        self._dir = args.directory

    def ensure_io_properties(self):
        if os.path.exists('io_properties.yaml'):
            print('Using existing io_properties file')
        else:
            print('Running iotune')
            subprocess.check_call([self._iotune, '--evaluation-directory', self._dir, '-c1', '--properties-file', 'io_properties.yaml'])


class job:
    def __init__(self, typ, req_size_kb, prl):
        self._typ = typ
        self._req_size = req_size_kb
        self._prl = prl
        self._shares = 100

    def prl(self):
        return self._prl

    def rqsz(self):
        return self._req_size

    def to_conf_entry(self, name):
        return {
            'name': name,
            'shards': 'all',
            'type': self._typ,
            'shard_info': {
                'parallelism': self._prl,
                'reqsize': f'{self._req_size}kB',
                'shares': self._shares
            }
        }


class io_tester:
    def __init__(self, args):
        self._jobs = []
        self._duration = args.duration
        self._io_tester = args.bdir + '/apps/io_tester/io_tester'
        self._dir = args.directory
        self._use_fraction = 0.8

    def add_job(self, name, job):
        self._jobs.append(job.to_conf_entry(name))

    def _setup_data_sizes(self):
        du = shutil.disk_usage(self._dir)
        one_job_space_mb = int(du.free * self._use_fraction / len(self._jobs) / (100*1024*1024)) * 100 # round down to 100MB
        if one_job_space_mb > 8 * 1024:
            one_job_space_mb = 8 * 1024
        for j in self._jobs:
            j['data_size'] = f'{one_job_space_mb}MB'

    def run(self):
        if not self._jobs:
            raise 'Empty jobs'

        self._setup_data_sizes()
        yaml.dump(self._jobs, open('conf.yaml', 'w'))
        self._proc = subprocess.Popen([self._io_tester, '-c1',
                                        '--storage', self._dir,
                                        '--conf', 'conf.yaml',
                                        '--duration', f'{self._duration}',
                                        '--io-properties-file', 'io_properties.yaml'], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        res = self._proc.communicate()
        res = res[0].split(b'---\n')[1]
        return yaml.safe_load(res)[0]


def show_stat_line(res, name):
    st = res[name]
    throughput = st['throughput']
    iops = st['IOPS']
    lats = st['latencies']
    stats = st['stats']

    def xtimes(st, nm):
        return int(st[nm]/st["total_requests"] * 1000000)

    print(f'{name}: throughput {throughput} kb/s  IOPS {iops}  lat.95 {lats["p0.95"]}ns  qtime {xtimes(stats, "io_queue_total_delay_sec")}ns  xtime {xtimes(stats, "io_queue_total_exec_sec")}ns')


print('Pure write')
m = io_tester(args)
m.add_job('write', job('seqwrite', 64, 64))
res = m.run()
show_stat_line(res, 'write')

print('Pure read')
m = io_tester(args)
m.add_job('read', job('randread', 4, 2 ** 9))
res = m.run()
show_stat_line(res, 'read')

for prl in [ 2 ** i for i in range(0, 8) ]:
    print(f'Write vs {prl}x Read')
    m = io_tester(args)
    m.add_job('write', job('seqwrite', 64, 64))
    m.add_job('read', job('randread', 4, prl))
    res = m.run()
    show_stat_line(res, 'write')
    show_stat_line(res, 'read')

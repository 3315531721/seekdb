#!/usr/bin/env python3
"""性能基准: 10,000 行 × 128 维, 1000 次 TopK 查询 (K=10), 一半带过滤。
模拟赛题 3.3 的小型性能 benchmark。
"""
import json, os, random, subprocess, sys, tempfile, time

DIM = 128
N = 10000
Q = 1000
K = 10
SEED = 42

def gen_vec(rng):
    return [round(rng.uniform(-1, 1), 5) for _ in range(DIM)]

def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else './minivectordb'
    data_dir = tempfile.mkdtemp(prefix='mvdb_bench_')
    proc = subprocess.Popen([binary, '--data-dir', data_dir],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, text=True, bufsize=1)
    assert json.loads(proc.stdout.readline()) == {'type': 'ready'}
    rid = [0]
    def send(sql, mode='exact'):
        rid[0] += 1
        proc.stdin.write(json.dumps({'id': rid[0], 'op': 'execute', 'sql': sql, 'search_mode': mode}) + '\n')
        proc.stdin.flush()
        line = proc.stdout.readline()
        if not line:
            raise RuntimeError('no response: ' + proc.stderr.read()[:2000])
        return json.loads(line)

    rng = random.Random(SEED)
    t0 = time.time()
    print(send('CREATE TABLE bench (id BIGINT PRIMARY KEY, category VARCHAR(16), emb VECTOR(128));'))
    # 批量写入: 每批 100 条
    write_t0 = time.time()
    for start in range(0, N, 100):
        vals = []
        for i in range(start, min(start + 100, N)):
            v = gen_vec(rng)
            cat = 'c%d' % (i % 10)
            vals.append("(%d,'%s','[%s]')" % (i, cat, ','.join(str(x) for x in v)))
        r = send('INSERT INTO bench VALUES ' + ','.join(vals) + ';')
        if not r.get('ok'):
            print('INSERT FAIL', r); return 1
    write_elapsed = time.time() - write_t0
    print(f'写入 {N} 行耗时: {write_elapsed:.3f}s  ({N/write_elapsed:.0f} rows/s)')

    # 查询
    queries = []
    for j in range(Q):
        v = gen_vec(rng)
        if j % 2 == 0:
            cat = 'c%d' % (j % 10)
            queries.append(("SELECT id, L2_DISTANCE(emb, '[%s]') AS distance FROM bench WHERE category = '%s' ORDER BY distance LIMIT %d;" % (','.join(str(x) for x in v), cat, K)))
        else:
            queries.append(("SELECT id, L2_DISTANCE(emb, '[%s]') AS distance FROM bench ORDER BY distance LIMIT %d;" % (','.join(str(x) for x in v), K)))
    query_t0 = time.time()
    bad = 0
    for q in queries:
        r = send(q)
        if not r.get('ok'):
            bad += 1
            if bad < 3: print('QUERY FAIL', r)
    query_elapsed = time.time() - query_t0
    print(f'查询 {Q} 次耗时: {query_elapsed:.3f}s  ({Q/query_elapsed:.1f} qps, 失败 {bad})')

    rid[0] += 1
    proc.stdin.write(json.dumps({'id': rid[0], 'op': 'shutdown'}) + '\n')
    proc.stdin.flush()
    proc.stdout.readline()
    proc.wait(timeout=60)
    print(f'总耗时: {time.time()-t0:.3f}s')
    # 数据目录大小
    total = sum(os.path.getsize(os.path.join(data_dir, f)) for f in os.listdir(data_dir))
    print(f'数据目录: {total/1024/1024:.1f} MiB')
    return 0

if __name__ == '__main__':
    sys.exit(main())

#!/usr/bin/env python3
"""本地跑官方 mysqltest 公开用例 (JSONL 协议, 直接启动本地二进制)。
用法: python3 tests/run_cases.py [--bin ./minivectordb] [--cases /tmp/seekdb_probe/mysqltest] [--only sql_crud]
"""
import argparse, json, os, shutil, subprocess, sys, tempfile

def load(path):
    with open(path, encoding='utf-8') as f:
        return json.load(f)

def approx(a, b):
    if isinstance(a, float) or isinstance(b, float):
        try:
            fa, fb = float(a), float(b)
        except (TypeError, ValueError):
            return a == b
        return abs(fa - fb) <= 1e-5 or abs(fa - fb) <= 1e-5 * max(1.0, abs(fb))
    return a == b

def cmp_rows(actual, expected, unordered=False):
    if len(actual) != len(expected):
        return False
    def eq(r1, r2):
        if len(r1) != len(r2):
            return False
        return all(approx(x, y) for x, y in zip(r1, r2))
    if unordered:
        rem = list(expected)
        for r in actual:
            for i, e in enumerate(rem):
                if eq(r, e):
                    rem.pop(i)
                    break
            else:
                return False
        return True
    return all(eq(a, e) for a, e in zip(actual, expected))

def check_assertion(actual_resp, expect):
    """actual_resp: 完整响应 dict; expect: .result 中的一条断言"""
    if 'error' in expect:
        if actual_resp.get('ok') is not False:
            return "期望失败但返回 ok"
        err = actual_resp.get('error') or {}
        code = err.get('code')
        if not code or not isinstance(code, str):
            return f"错误码为空或非字符串: {code!r}"
        exp = expect['error']
        if exp and exp != code:
            return f"错误码不符: 期望 {exp}, 实际 {code}"
        return None
    if actual_resp.get('ok') is not True:
        return f"期望成功但 ok={actual_resp.get('ok')!r} err={actual_resp.get('error')}"
    if 'affected_rows' in expect:
        if actual_resp.get('affected_rows') != expect['affected_rows']:
            return f"affected_rows 不符: 期望 {expect['affected_rows']}, 实际 {actual_resp.get('affected_rows')}"
    if 'columns' in expect:
        if actual_resp.get('columns') != expect['columns']:
            return f"columns 不符: 期望 {expect['columns']}, 实际 {actual_resp.get('columns')}"
    if 'rows' in expect:
        if not cmp_rows(actual_resp.get('rows', []), expect['rows'], expect.get('unordered', False)):
            return f"rows 不符:\n  期望 {expect['rows']}\n  实际 {actual_resp.get('rows')}"
    return None

def run_case(bin_path, case_dir, case_id, tmp_root):
    tpath = os.path.join(case_dir, 'cases', case_id + '.test')
    rpath = os.path.join(case_dir, 'cases', case_id + '.result')
    sql_lines = [l.rstrip('\n') for l in open(tpath, encoding='utf-8') if l.strip()]
    expects = load(rpath)
    data_dir = os.path.join(tmp_root, case_id + '_data')
    os.makedirs(data_dir, exist_ok=True)

    proc = subprocess.Popen([bin_path, '--data-dir', data_dir],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True, bufsize=1)
    errors = []
    next_id = 1
    mode = 'exact'
    exp_i = 0

    def send(obj):
        proc.stdin.write(json.dumps(obj, ensure_ascii=False) + '\n')
        proc.stdin.flush()
        line = proc.stdout.readline()
        if not line:
            raise RuntimeError('进程无响应/已退出: ' + (proc.stderr.read() or '')[:2000])
        return json.loads(line)

    # 握手
    ready = proc.stdout.readline()
    try:
        r = json.loads(ready)
    except Exception:
        errors.append(f'握手不是JSON: {ready!r}')
        r = None
    if r != {'type': 'ready'}:
        errors.append(f'握手不符: {r!r}')

    for raw in sql_lines:
        line = raw.strip()
        if line.startswith('--'):
            cmd = line[2:].strip()
            if cmd == 'restart':
                try:
                    resp = send({'id': next_id, 'op': 'shutdown'}); next_id += 1
                    proc.wait(timeout=30)
                except Exception as e:
                    errors.append(f'restart shutdown 失败: {e}')
                if proc.returncode not in (0, None):
                    errors.append(f'restart 退出码 {proc.returncode}')
                proc = subprocess.Popen([bin_path, '--data-dir', data_dir],
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE, text=True, bufsize=1)
                ready = proc.stdout.readline()
                try:
                    r = json.loads(ready)
                except Exception:
                    errors.append(f'restart 握手不是JSON: {ready!r}')
                    r = None
                if r != {'type': 'ready'}:
                    errors.append(f'restart 握手不符: {r!r}')
                mode = 'exact'   # 每个用例最初为 exact; restart 后沿用? 官方: 每个用例最初 exact
            elif cmd.startswith('mode'):
                parts = cmd.split()
                if len(parts) == 2 and parts[0] == 'mode':
                    mode = parts[1]
            continue

        expect = expects[exp_i] if exp_i < len(expects) else {}
        exp_i += 1
        try:
            resp = send({'id': next_id, 'op': 'execute', 'sql': line, 'search_mode': mode})
        except Exception as e:
            errors.append(f'[{line[:60]}] 请求失败: {e}')
            break
        next_id += 1
        if resp.get('id') != next_id - 1:
            errors.append(f'[{line[:60]}] id 不符: {resp.get("id")}')
        msg = check_assertion(resp, expect)
        if msg:
            errors.append(f'[{line[:60]}] {msg}')

    # shutdown
    try:
        send({'id': next_id, 'op': 'shutdown'})
        proc.wait(timeout=30)
    except Exception as e:
        errors.append(f'shutdown 失败: {e}')
        proc.kill()
    if exp_i != len(expects):
        errors.append(f'断言数不符: 执行 {exp_i} 条, 期望 {len(expects)} 条')
    return errors

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--bin', default='./minivectordb')
    ap.add_argument('--cases', default='tests/official')
    ap.add_argument('--only', default=None)
    args = ap.parse_args()
    case_root = args.cases
    if not os.path.isdir(case_root):
        case_root = 'tests/official'
    if not os.path.isdir(case_root):
        case_root = '/tmp/seekdb_probe/mysqltest'
    manifest = load(os.path.join(case_root, 'manifest.json'))
    cases = [c['id'] for c in manifest['cases']]
    if args.only:
        cases = [args.only]
    tmp_root = tempfile.mkdtemp(prefix='mvdb_test_')
    passed = failed = 0
    all_err = []
    for cid in cases:
        errs = run_case(args.bin, case_root, cid, tmp_root)
        if errs:
            failed += 1
            print(f'❌ {cid}')
            for e in errs[:8]:
                print('    ', e)
            all_err.append((cid, errs))
        else:
            passed += 1
            print(f'✅ {cid}')
    print(f'\n===== {passed} passed, {failed} failed / {len(cases)} =====')
    if failed:
        sys.exit(1)

if __name__ == '__main__':
    main()

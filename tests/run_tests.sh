#!/usr/bin/env bash
# MiniVectorDB 本地冒烟测试
# 前置条件：先在项目根目录执行 make，生成 ./minivectordb。
# 用法：bash tests/run_tests.sh
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
BIN="${ROOT_DIR}/minivectordb"
SQL_FILE="${ROOT_DIR}/tests/smoke.sql"
DATA_DIR="/tmp/mvdb_test_data"

die() {
    echo "[FAIL] $*" >&2
    exit 1
}

if [[ ! -x "${BIN}" ]]; then
    cat >&2 <<EOF
[ERROR] 未找到可执行文件：${BIN}
请先在项目根目录执行构建：
  cd "${ROOT_DIR}"
  make -j4
然后再运行：
  bash tests/run_tests.sh
EOF
    exit 2
fi

if [[ ! -f "${SQL_FILE}" ]]; then
    die "缺少测试 SQL 文件：${SQL_FILE}"
fi

if ! command -v python3 >/dev/null 2>&1; then
    die "需要 python3 将 smoke.sql 转换为 JSONL 协议请求"
fi

TMP_DIR="$(mktemp -d /tmp/mvdb_smoke.XXXXXX)"
cleanup() {
    rm -rf -- "${TMP_DIR}"
}
trap cleanup EXIT

# 每次从空数据目录开始；路径固定为题目指定的 /tmp/mvdb_test_data。
rm -rf -- "${DATA_DIR}"
mkdir -p "${DATA_DIR}"

echo "==> 生成 JSONL 请求"
python3 - "${SQL_FILE}" "${TMP_DIR}/phase1.jsonl" "${TMP_DIR}/manifest.json" <<'PY'
import json
import sys

sql_path, req_path, manifest_path = sys.argv[1], sys.argv[2], sys.argv[3]
statements = []
with open(sql_path, "r", encoding="utf-8") as f:
    for raw in f:
        line = raw.strip()
        if not line or line.startswith("--"):
            continue
        statements.append(line)

if not statements:
    raise SystemExit("smoke.sql 中没有可执行 SQL")

requests = []
for idx, sql in enumerate(statements, start=1):
    requests.append({
        "id": idx,
        "op": "execute",
        "sql": sql,
        "search_mode": "exact",
    })
requests.append({"id": len(statements) + 1, "op": "shutdown"})

with open(req_path, "w", encoding="utf-8") as f:
    for req in requests:
        f.write(json.dumps(req, ensure_ascii=False, separators=(",", ":")) + "\n")

with open(manifest_path, "w", encoding="utf-8") as f:
    json.dump({"sql_count": len(statements)}, f)
print(f"    {len(statements)} 条 SQL -> phase1.jsonl")
PY

echo "==> 阶段 1：建表、写入、查询、事务、向量检索"
set +e
"${BIN}" --data-dir "${DATA_DIR}" \
    < "${TMP_DIR}/phase1.jsonl" \
    > "${TMP_DIR}/phase1.out" \
    2> "${TMP_DIR}/phase1.err"
PHASE1_RC=$?
set -e

if [[ ${PHASE1_RC} -ne 0 ]]; then
    echo "[FAIL] 阶段 1 进程退出码 ${PHASE1_RC}" >&2
    echo "--- stdout ---" >&2
    cat "${TMP_DIR}/phase1.out" >&2 || true
    echo "--- stderr ---" >&2
    cat "${TMP_DIR}/phase1.err" >&2 || true
    exit 1
fi

echo "==> 校验阶段 1 响应"
python3 - "${TMP_DIR}/phase1.out" "${TMP_DIR}/manifest.json" <<'PY'
import json
import sys

out_path, manifest_path = sys.argv[1], sys.argv[2]
with open(manifest_path, "r", encoding="utf-8") as f:
    sql_count = json.load(f)["sql_count"]

with open(out_path, "r", encoding="utf-8") as f:
    lines = [line.strip() for line in f if line.strip()]

if not lines:
    raise SystemExit("FAIL: 没有收到任何 stdout 响应")

try:
    objects = [json.loads(line) for line in lines]
except json.JSONDecodeError as exc:
    raise SystemExit(f"FAIL: stdout 不是合法 JSONL: {exc}")

if objects[0].get("type") != "ready":
    raise SystemExit(f"FAIL: 首行不是 ready 标识: {objects[0]!r}")

expected_lines = 1 + sql_count + 1  # ready + SQL 响应 + shutdown 响应
if len(objects) != expected_lines:
    raise SystemExit(f"FAIL: stdout 行数应为 {expected_lines}，实际 {len(objects)}；不应有额外输出")

responses = {}
for obj in objects:
    if "id" in obj:
        responses[obj["id"]] = obj

expected_ids = list(range(1, sql_count + 2))  # SQL 1..n + shutdown n+1
missing = [i for i in expected_ids if i not in responses]
if missing:
    raise SystemExit(f"FAIL: 缺少响应 id: {missing}")

for i in expected_ids:
    if not responses[i].get("ok"):
        raise SystemExit(f"FAIL: 请求 {i} 返回错误: {responses[i].get('error')!r}")


def by_id(resp):
    columns = resp.get("columns", [])
    if "id" not in columns:
        raise SystemExit(f"FAIL: 结果集缺少 id 列，columns={columns!r}")
    id_pos = columns.index("id")
    result = {}
    for row in resp.get("rows", []):
        result[int(row[id_pos])] = row
    return columns, result


def ids(resp):
    return list(by_id(resp)[1].keys())


# 3: 初始三行，且 SELECT * 返回 schema 顺序
cols, rows = by_id(responses[3])
if cols != ["id", "title", "embedding"]:
    raise SystemExit(f"FAIL: SELECT * 列顺序应为 [id,title,embedding]，实际 {cols!r}")
if ids(responses[3]) != [1, 2, 3]:
    raise SystemExit(f"FAIL: 初始查询 id 应为 [1,2,3]，实际 {ids(responses[3])}")

# 4: 精确 TopK，距离最近的是 id=1，其次 id=2
if ids(responses[4]) != [1, 2]:
    raise SystemExit(f"FAIL: 向量 TopK 应为 [1,2]，实际 {ids(responses[4])}")

# 5: 过滤后只在 Alpha 内检索
if ids(responses[5]) != [1]:
    raise SystemExit(f"FAIL: 过滤向量检索应为 [1]，实际 {ids(responses[5])}")

# 7: UPDATE 生效
cols7, rows7 = by_id(responses[7])
title_pos = cols7.index("title")
if rows7[2][title_pos] != "BetaUpdated":
    raise SystemExit(f"FAIL: UPDATE 后 id=2 title 应为 BetaUpdated，实际 {rows7[2][title_pos]!r}")

# 9: DELETE 生效
if ids(responses[9]) != [1, 2]:
    raise SystemExit(f"FAIL: DELETE 后应为 [1,2]，实际 {ids(responses[9])}")

# 12: 事务内可见自己的插入
if 4 not in ids(responses[12]):
    raise SystemExit("FAIL: 事务内应看到 RollbackMe(id=4)")

# 14: ROLLBACK 后不可见
if 4 in ids(responses[14]):
    raise SystemExit("FAIL: ROLLBACK 后仍看到 RollbackMe(id=4)")

# 19: COMMIT 后可见，且更新保留
if ids(responses[19]) != [1, 2, 5]:
    raise SystemExit(f"FAIL: COMMIT 后应为 [1,2,5]，实际 {ids(responses[19])}")

cols19, rows19 = by_id(responses[19])
title_pos19 = cols19.index("title")
if rows19[2][title_pos19] != "BetaFinal":
    raise SystemExit(f"FAIL: COMMIT 后 id=2 title 应为 BetaFinal，实际 {rows19[2][title_pos19]!r}")
if rows19[5][title_pos19] != "Committed":
    raise SystemExit("FAIL: COMMIT 后未看到 Committed(id=5)")

# 20: 提交后的向量检索
if ids(responses[20]) != [1, 2]:
    raise SystemExit(f"FAIL: 提交后向量查询应为 [1,2]，实际 {ids(responses[20])}")

cols20, rows20 = by_id(responses[20])
if "dist" not in cols20:
    raise SystemExit(f"FAIL: 向量查询缺少 dist 列: {cols20!r}")
dist_pos = cols20.index("dist")
d0 = float(rows20[1][dist_pos])
d1 = float(rows20[2][dist_pos])
if not (d0 <= d1):
    raise SystemExit(f"FAIL: 距离未按升序返回: {d0} > {d1}")
if abs(d0) > 1e-6:
    raise SystemExit(f"FAIL: 查询向量与 id=1 距离应为 0，实际 {d0}")

print(f"    OK: {sql_count} 条 SQL + shutdown 全部成功")
PY

echo "==> 阶段 2：复用 /tmp/mvdb_test_data 重启并验证持久化"
python3 - "${TMP_DIR}/phase2.jsonl" <<'PY'
import json
import sys

requests = [
    {"id": 101, "op": "execute", "search_mode": "exact",
     "sql": "SELECT id, title FROM docs ORDER BY id;"},
    {"id": 102, "op": "execute", "search_mode": "exact",
     "sql": "SELECT id, title, L2_DISTANCE(embedding, '[1.0, 0.0, 0.0, 0.0]') AS dist FROM docs ORDER BY dist LIMIT 2;"},
    {"id": 103, "op": "shutdown"},
]
with open(sys.argv[1], "w", encoding="utf-8") as f:
    for req in requests:
        f.write(json.dumps(req, ensure_ascii=False, separators=(",", ":")) + "\n")
PY

set +e
"${BIN}" --data-dir "${DATA_DIR}" \
    < "${TMP_DIR}/phase2.jsonl" \
    > "${TMP_DIR}/phase2.out" \
    2> "${TMP_DIR}/phase2.err"
PHASE2_RC=$?
set -e

if [[ ${PHASE2_RC} -ne 0 ]]; then
    echo "[FAIL] 阶段 2 进程退出码 ${PHASE2_RC}" >&2
    echo "--- stdout ---" >&2
    cat "${TMP_DIR}/phase2.out" >&2 || true
    echo "--- stderr ---" >&2
    cat "${TMP_DIR}/phase2.err" >&2 || true
    exit 1
fi

echo "==> 校验重启后的持久化响应"
python3 - "${TMP_DIR}/phase2.out" <<'PY'
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as f:
    objects = [json.loads(line) for line in f if line.strip()]

if not objects or objects[0].get("type") != "ready":
    raise SystemExit("FAIL: 重启后没有 ready 标识")
if len(objects) != 4:
    raise SystemExit(f"FAIL: 重启后 stdout 应为 4 行（ready + 2 条查询 + shutdown），实际 {len(objects)} 行")

responses = {obj["id"]: obj for obj in objects if "id" in obj}
for i in (101, 102, 103):
    if i not in responses:
        raise SystemExit(f"FAIL: 缺少响应 id={i}")
    if not responses[i].get("ok"):
        raise SystemExit(f"FAIL: 请求 {i} 失败: {responses[i].get('error')!r}")


def parse_rows(resp):
    columns = resp.get("columns", [])
    if "id" not in columns:
        raise SystemExit(f"FAIL: columns 缺少 id: {columns!r}")
    pos = columns.index("id")
    return columns, {int(row[pos]): row for row in resp.get("rows", [])}


cols, rows = parse_rows(responses[101])
if list(rows.keys()) != [1, 2, 5]:
    raise SystemExit(f"FAIL: 重启后 id 应为 [1,2,5]，实际 {list(rows.keys())}")
if "title" not in cols:
    raise SystemExit("FAIL: 重启查询缺少 title 列")
title_pos = cols.index("title")
if rows[5][title_pos] != "Committed":
    raise SystemExit("FAIL: 重启后未保留 Committed(id=5)")
if rows[2][title_pos] != "BetaFinal":
    raise SystemExit("FAIL: 重启后未保留 BetaFinal 更新")

cols2, rows2 = parse_rows(responses[102])
if list(rows2.keys()) != [1, 2]:
    raise SystemExit(f"FAIL: 重启后向量查询应为 [1,2]，实际 {list(rows2.keys())}")

print("    OK: 重启后数据与向量检索一致")
PY

echo
echo "全部冒烟测试通过。"
echo "数据目录保留在：${DATA_DIR}"

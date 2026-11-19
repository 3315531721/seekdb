# MiniVectorDB 设计说明

本文记录 MiniVectorDB 的存储布局、向量检索方案、事务与恢复流程，以及当前实现边界。设计目标对应 2026 OceanBase 数据库大赛赛题一：单机、单进程、单客户端连接、最多一个显式事务，不实现分布式执行和完整 SQL 标准。

> 文档状态：基于当前仓库实现（2026-11-19）整理。实现细节以 `src/` 下源码为准；后续若调整协议或存储格式，应同步更新本文。

## 1. 总体架构

```text
stdin (JSONL 请求)
      │
      ▼
protocol.cpp  ── 解析请求、输出 ready / 结果 / 错误
      │
      ▼
parser.cpp    ── 手写词法器 + 递归下降，生成 AST
      │
      ▼
executor.cpp  ── 类型校验、条件过滤、投影、排序、TopK
      │
      ▼
storage.cpp   ── 页式表文件 + 主键索引 + 写集 + WAL + 恢复
      │
      ▼
stdout (JSONL 响应) / stderr (日志)
```

模块职责：

| 模块 | 职责 |
| --- | --- |
| `protocol.cpp` | 维护常驻进程；读取 stdin 的 JSONL 请求；输出 `ready`、结果集、DML 影响行数和错误；处理 `shutdown` |
| `parser.cpp` | 解析题面 SQL 子集；拒绝不支持语法；负责字符串、整数、向量字面量的基础解析 |
| `executor.cpp` | 执行 DDL/DML/SELECT；校验列类型与向量维度；在 `WHERE` 过滤后的集合上完成 TopK |
| `storage.cpp` | 表目录、4096 字节页式数据文件、主键定位索引、事务写集、WAL、提交与恢复 |
| `json.cpp` | 无外部依赖的 JSON 解析与序列化，供协议层使用 |

## 2. 磁盘布局

所有文件位于启动参数 `--data-dir` 指定的目录，评测容器中固定为 `/data`。

```text
/data/
├── catalog.dat          # 表定义目录
├── wal.log              # 预写日志
├── docs.tbl             # 每张表一个数据文件
└── <table>.tbl
```

### 2.1 `catalog.dat`

```text
[magic:u32 = "MVDC"][format_version:u32][table_count:u32]
重复 table_count 次:
  [table schema]
```

表 schema 的序列化字段为：

```text
[table_name:string]
[column_count:u32]
重复 column_count 次:
  [name:string][type:u8][varchar_len:u32][vector_dim:u32][is_primary:u8]
[primary_idx:u32][vector_idx:u32]
```

`CREATE TABLE` 成功后刷新并 `fsync` catalog。字符串统一使用 `[u32 长度][字节内容]` 编码。

### 2.2 表数据文件

每张表一个 `<table>.tbl`：

```text
[magic:u32 = "MVDT"][format_version:u32][schema_len:u32][schema_bytes]
随后是连续排列的 4096 字节页
```

页内记录采用追加写，不跨页：

```text
[u32 body_len][i64 primary_key][列数据...]
```

列数据按 schema 顺序编码：

- `BIGINT`：8 字节小端整数；
- `VARCHAR`：`[u32 字节长度][UTF-8 字节]`；
- `VECTOR`：`[u32 维度][float32 × 维度]`，每个 float32 按 4 字节小端保存。

当当前页剩余空间不足时新开一页。单行必须小于一页，超过时返回约束错误。删除和更新不原地修改旧记录：更新通过写集记录新行，删除通过写集记录 tombstone；提交时在必要时压实表文件，仅保留存活行和新版本行。

### 2.3 `wal.log`

```text
[magic:u32 = "MVDW"][format_version:u32]
重复记录:
  [payload_len:u32][crc32:u32][payload:bytes]
```

payload 公共头：

```text
[txn_id:u64][op:u8]
```

操作类型：

| op | 含义 | 其余字段 |
| --- | --- | --- |
| 1 | INSERT / UPDATE | `[table:string][pk:i64][按 schema 编码的列数据]` |
| 2 | UPDATE | 与 INSERT 相同（按 upsert 语义重放） |
| 3 | DELETE | `[table:string][pk:i64]` |
| 4 | COMMIT | 无 |
| 5 | ABORT | 无 |
| 6 | DDL_CREATE | `[table schema]` |

每条记录带 CRC32；读取到长度不足、CRC 不匹配或未知记录时停止重放，视为崩溃留下的尾部残记录。

## 3. 内存结构与数据访问

- `StorageEngineImpl` 持有 `table_name -> TableEntry` 映射；
- 每个 `TableDataFile` 维护 `primary_key -> RowLoc{page_id, offset, len}` 的主键定位索引；
- 扫描时按主键排序后逐行 `pread`，行体（含向量）不常驻内存；
- 事务写集为 `table -> pk -> {Row, deleted}`，在基表之上叠加可见性。

查询层另有表快照缓存：单表估算大小不超过 256 MiB（可由环境变量 `MVDB_SNAPSHOT_LIMIT_MB` 调整）时，第一次查询把该表可见行整体缓存，后续查询避免重复读盘；超过上限时标记为超内存表并降级为流式扫描。任何 DML 或事务边界都会使快照缓存失效，避免读到旧数据。这样既能让小表反复 TopK 查询保持较低延迟，也不会把大于内存预算的表强行全量载入。

普通查询流程：

1. `Table::scan` 或表快照取得可见记录；
2. 执行 `WHERE` 条件过滤；
3. 按投影构造结果列；
4. 有 `ORDER BY` 时排序，距离并列按主键升序；无 `ORDER BY` 时按主键升序输出，保证结果确定；
5. 应用 `LIMIT`；大表按 `LIMIT` 使用有界 Top-N 结构，内存为 O(limit)。

向量查询流程：

1. 先按 `WHERE` 中的普通列条件过滤，得到候选行集合；
2. 对候选行的向量与查询向量计算 L2 欧氏距离；
3. 按 `(distance ASC, primary_key ASC)` 排序；
4. 取 `LIMIT` / TopK 指定的前 K 条；大表使用 O(K) 有界最大堆流式维护；
5. 结果不足 K 条时返回全部候选。

这种布局把表定义、行定位和行数据分离，行数据按需读取；但主键定位表本身仍与行数成正比，属于当前实现的已知内存边界。

## 4. 向量检索方案

### 4.1 精确检索

当前所有向量查询均走精确扫描，区别只在执行结构：

- 不超过快照上限的表：一次性建立可见行快照，在内存中完成过滤、距离计算和排序；
- 超过快照上限的表：按页流式扫描，使用大小为 K 的最大堆维护当前 TopK，额外内存 O(K)。

两种路径共享同一个语义：`WHERE` 过滤在距离计算之前完成，距离按 `float32` 存储值计算、以 `double` 累加，排序键为 `(distance ASC, primary_key ASC)`，结果不足 K 条时返回全部候选。因此不会出现“先全表取 K 条再过滤”的错误语义，也不会因为走快照而绕过事务可见性。

支持两种写法：

```sql
-- 投影中计算距离并排序
SELECT id, L2_DISTANCE(embedding, '[1,0,0,0]') AS dist
FROM docs
ORDER BY dist
LIMIT 2;

-- WHERE 中显式写距离条件
SELECT id FROM docs
WHERE L2_DISTANCE(embedding, '[1,0,0,0]') <= 2
ORDER BY id;
```

### 4.2 近似检索与索引

`CREATE VECTOR INDEX index_name ON table_name(vector_column)` 当前只做语法接受和元信息占位，不构建独立 ANN 图或量化索引；`search_mode=ann` 仍由同一套精确扫描执行，因此返回结果正确，但没有近似索引带来的性能收益。这是当前实现最重要的性能限制。

若后续加入独立索引，设计约束为：

- 索引只是基表数据的派生结构，删除/更新必须同步维护；
- 索引重建完成前不得报告 ready；
- exact 模式永远可以绕过索引走精确扫描；
- 带过滤的 ANN 必须先在满足条件的集合内检索，不能先全表取 K 再过滤。

## 5. 事务处理

### 5.1 事务状态

- 同一时刻最多一个显式事务；
- `BEGIN` 建立事务，`COMMIT` 提交，`ROLLBACK` 丢弃；
- 未显式 `BEGIN` 时，每条修改语句自动形成一个隐式事务，成功即提交，失败即回滚；
- `BEGIN` 时已有事务、无事务时 `COMMIT`/`ROLLBACK` 返回事务状态错误。

### 5.2 可见性

事务内查询看到：

```text
基表可见行
  + 本事务插入或更新的行
  - 本事务删除的行
```

`write_set_` 中的更新行覆盖基表旧版本；删除标记使旧版本不可见；新增主键在扫描尾部按主键排序补入。因此普通查询和向量检索共享同一套可见性，不会出现“表里能看到、向量检索看不到”的不一致。

### 5.3 语句级原子性

- INSERT 先完成整批行的列数、类型、VARCHAR 长度、向量维度及有限性校验，再写入写集；任一行失败不产生部分写入。
- UPDATE 先校验 SET 列与值，再收集匹配行和新行，最后统一写入写集。
- DELETE 先收集命中主键，再统一写 tombstone。
- 显式事务内某条语句失败时，该语句不留下部分修改，之前成功的语句仍可继续提交或回滚。

### 5.4 DDL 与事务

显式事务内的 `CREATE TABLE` 先创建表文件和内存表项，并把 schema 放入 `pending_ddl_`；提交时写 catalog，回滚时删除新建表文件。这样避免 DDL 在事务回滚后残留。

## 6. 提交持久化

`COMMIT` 的顺序：

1. 将本事务所有写集操作编码为 WAL 记录，末尾追加 `COMMIT` 记录；
2. 一次写入 WAL，并 `fsync(wal.log)`；
3. 将写集应用到 `.tbl` 数据文件：追加新行、必要时压实删除/更新；
4. 对表文件 `fsync`；
5. 截断 WAL 到文件头（checkpoint）；
6. 清理事务状态，向调用方返回成功。

关键持久化点是第 2 步：只有 WAL 的 `COMMIT` 记录完成 `fsync` 后，才会继续应用数据并最终返回成功。因此已经收到成功响应的提交不会因进程被杀而丢失；如果进程在 WAL fsync 之后、数据文件落盘之前被杀，重启会从 WAL 重放完整事务。

## 7. 恢复流程

`StorageEngine::open()`：

1. 创建或打开数据目录；
2. 读取 `catalog.dat`，恢复表 schema；
3. 打开每张表的 `.tbl`，重建主键定位索引；
4. 打开 `wal.log`，扫描完整记录；
5. 按 `txn_id` 聚合事务：
   - 有 `COMMIT` 记录的事务：完整重放；
   - 没有 `COMMIT` 记录或遇到 `ABORT` 的事务：整段丢弃；
6. 重放 DDL（建表），再重放数据（insert/update 为 upsert，delete 为 tombstone）；
7. 将重放结果落盘并 `fsync`；
8. checkpoint 截断 WAL；
9. 完成以上步骤后才输出 `{"type":"ready"}`。

恢复只依赖 WAL 和已落盘的数据文件，不依赖内存中的旧状态。重复恢复是幂等的：已存在于数据文件中的主键会走压实/覆盖路径，不会产生重复行；提交未完成的事务不会部分生效。

## 8. 错误处理

- 解析器和执行器抛出 `DbError`，协议层转换为 `{"ok":false,"error":{"code":"...","message":"..."}}`；
- 错误码语义名包括 `syntax_error`、`table_not_found`、`table_exists`、`column_not_found`、`duplicate_key`、`type_mismatch`、`dimension_mismatch`、`invalid_vector`、`varchar_too_long`、`unsupported_sql`、`transaction_error`、`constraint_violation`、`internal_error`、`io_error`；
- 单条语句错误不会终止进程，后续请求可继续执行；
- 启动阶段 I/O 或 catalog/WAL 损坏导致无法打开时，进程写 stderr 并返回非零退出码；
- 所有日志写 stderr，stdout 只保留协议响应。

## 9. 已实现功能与已知限制

### 9.1 已实现功能

- C++17、无第三方数据库/向量索引依赖；
- `CREATE TABLE`：`BIGINT`、`VARCHAR(n)`、`VECTOR(n)`、单 BIGINT 主键、最多一个向量列；
- 多行 `INSERT`、按条件 `UPDATE`、按条件 `DELETE`；
- `SELECT *` / 列投影、`= < <= > >=`、`AND`、`ORDER BY`、`LIMIT`；
- `L2_DISTANCE` 精确 TopK，先过滤后计算，距离并列按主键升序；
- `BEGIN` / `COMMIT` / `ROLLBACK`，事务内读己写，语句级原子性；
- 自研 4096 字节页式表文件、catalog、WAL、CRC 校验、提交 fsync、启动恢复、重复恢复；
- 小表快照缓存（默认上限 256 MiB，可由 `MVDB_SNAPSHOT_LIMIT_MB` 调整）与大表流式 TopK/TopN；
- JSONL stdin/stdout 常驻协议、`ready` 就绪标识、`shutdown` 正常关闭；
- Docker 多阶段构建与本地冒烟测试脚本。

### 9.2 已知限制

1. **ANN 索引未实际建立**：`CREATE VECTOR INDEX` 是元信息占位，`search_mode=ann` 仍执行精确扫描；正确性满足，但大数据量下没有近似索引加速。
2. **主键索引全内存**：每表维护 `pk -> RowLoc` 的哈希表，表行数很大时该索引本身会超过内存预算；行体不常驻内存，但主键定位结构仍是 O(N)。
3. **扫描需要排序主键列表**：每次全表扫描会复制并排序全部主键，内存峰值与行数成正比，尚未实现外部排序或分块游标。
4. **写集全内存**：大事务的所有新行和 tombstone 保存在内存中，超大大事务受内存限制。
5. **单事务、单客户端**：不支持并发事务、多连接或快照隔离。
6. **崩溃模型有限**：只针对进程被强制终止；不模拟断电、磁盘损坏、外部篡改，页级校验和尚未实现。
7. **SQL 子集有限**：不支持 NULL、JOIN、聚合、子查询、ALTER TABLE、外键、嵌套事务和主键更新；这些语句返回错误。
8. **协议验证仍需落地**：当前已按官方 `2026-competition-round2/STDIO_PROTOCOL.md` 实现 JSONL 的 ready/execute/shutdown 报文；提交前仍应使用题包自带的 `protocol_check` 做启动握手时序、逐请求 flush 和重启模式验证。

## 10. 验证记录

本地冒烟测试由 `tests/run_tests.sh` 执行，测试输入位于 `tests/smoke.sql`，覆盖：

- 建表与多行插入；
- `SELECT *`、普通列投影和 `ORDER BY`；
- 精确向量 TopK 与带普通列过滤的向量检索；
- `UPDATE`、`DELETE`；
- 显式事务内插入、`ROLLBACK` 后的不可见性；
- 显式事务内插入、`COMMIT` 后的可见性；
- 关闭进程并使用同一 `/data` 目录重启后的持久化检查。

本地验证记录（2026-11-19，macOS / Apple Silicon，仅作相对参考）：

| 验证项 | 命令 | 结果 |
| --- | --- | --- |
| 本地构建 | `make -j4` | 通过，生成 `minivectordb` |
| 冒烟测试 | `bash tests/run_tests.sh` | 20 条 SQL + shutdown + 重启持久化检查通过 |
| 官方公开用例 | `python3 tests/run_cases.py --cases /tmp/seekdb_probe/mysqltest` | 20 passed, 0 failed |
| 性能基准 | `python3 tests/bench.py ./minivectordb` | 约 5610 rows/s，约 2930 qps（10,000 行 × 128 维） |
| Docker 构建 | `docker build ...` | 本机无 Docker，未执行 |

测试脚本在二进制不存在时会提示先执行 `make`，不会把未运行当作通过。真实执行结果记录在 `agent/conversation.json` 的 `experiments` 字段；本地构建不等同于容器构建，Docker 镜像构建须在安装 Docker 的环境另行验证。

## 11. 查询性能设计（实现现状）

### 11.1 表快照缓存

`executor.cpp` 维护 `TableSnapshot`（主键数组 + 行副本），首次查询时按表构建：

- 默认上限 256 MiB（可用环境变量 `MVDB_SNAPSHOT_LIMIT_MB` 调整）。
- 表超过上限时不缓存，标记为「大表」，后续查询直接走流式路径。
- 任何 DDL/DML/事务边界（BEGIN/COMMIT/ROLLBACK）都会 `invalidate_snapshot_cache()`，
  保证快照不会读到未提交或已回滚的数据。

### 11.2 向量 TopK 的有界堆

- 缓存路径：当 `K < N/4` 时使用大小为 K 的最大堆（`std::priority_queue`），
  复杂度 O(N log K)，避免对全部 N 条命中做 O(N log N) 全排序。
- 大表流式路径：同样使用 O(K) 内存的有界堆，边扫描边淘汰，绝不把整表读入内存。
- 顺序语义不变：距离升序；距离并列按主键升序（`DIST_EPS = 1e-9`）。

### 11.3 普通查询的有界 Top-N

大表上带 `LIMIT` 的普通查询采用「保留至多 limit 行的有界集合 + 逐行替换」，
内存 O(limit)；不带 LIMIT 的查询语义上需要返回全表，属于少见场景。

### 11.4 距离计算

`l2_distance` 使用 float32 累加 + 4 路手工展开（`s0..s3` 四个累加器），
便于编译器自动向量化并提高指令级并行；返回前转 double 再开方。

### 11.5 实测（macOS / Apple Silicon，仅作相对参考）

| 阶段 | 写入 | 查询 (1000 次 TopK) |
| --- | --- | --- |
| 初版（每次查询全表读盘） | 4348 rows/s | 63 qps |
| 表快照缓存 | — | 1097 qps |
| + 有界堆 TopK | — | 1762 qps |
| + 距离 4 路展开 / -O3 | ~5000 rows/s | ~2850 qps |

10,000 行 × 128 维数据集：峰值 RSS 约 20 MB。

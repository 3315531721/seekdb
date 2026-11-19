# MiniVectorDB

MiniVectorDB 是 2026 OceanBase 数据库大赛赛题一「轻量向量数据库」的参赛实现。目标是在单机、单进程、单客户端连接的前提下，提供基础 SQL、持久化存储、事务、精确向量 TopK 检索与异常退出恢复能力。

评测入口是一个常驻进程：从标准输入（stdin）逐行读取 JSONL 请求，将 SQL 执行结果以 JSONL 写到标准输出（stdout），日志只写标准错误（stderr）。程序不启动网络监听，不需要暴露或映射端口。

## 1. 语言与版本

| 项目 | 版本 / 说明 |
| --- | --- |
| 语言标准 | C++17 |
| 编译器 | Ubuntu 24.04 的 `g++`（GCC 13 系列） |
| 构建工具 | GNU `make` |
| 基础镜像 | `ubuntu:24.04`，构建阶段与运行阶段均使用该版本 |
| 第三方数据库/索引库 | 不使用 SQLite、RocksDB、Faiss、VSAG、HNSWlib 等现成引擎 |

`Makefile` 使用 `g++ -std=c++17 -O3 -Wall -Wextra -Iinclude` 编译 `src/*.cpp`，生成仓库根目录下的 `minivectordb` 可执行文件。

## 2. 目录结构

```text
.
├── Dockerfile              # 多阶段构建：编译 C++17 程序并生成运行镜像
├── Makefile                # 本地构建入口
├── README.md               # 本文件
├── agent/
│   └── conversation.json   # Agent prompt / 学习证据材料
├── docs/
│   └── DESIGN.md           # 存储、索引、事务与恢复设计
├── include/                # 公共接口与类型
├── src/                    # 数据库实现
└── tests/
    ├── run_tests.sh        # 本地冒烟测试
    └── smoke.sql           # 冒烟测试 SQL 样例
```

## 3. 镜像构建

在项目根目录执行：

```bash
docker build --platform linux/amd64 -t minivectordb:local .
```

构建分两个阶段：

1. `build` 阶段安装 `build-essential`、`g++`、`make`，复制 `Makefile`、`include/`、`src/` 并执行 `make -j$(nproc)`；
2. 运行阶段只安装 `libstdc++6`，把编译好的 `minivectordb` 复制到 `/usr/local/bin/`，并创建 `/data`。

`--platform linux/amd64` 用于在非 x86_64 开发机上产出与正式评测一致的 Linux x86_64 镜像；在 x86_64 主机上可以省略。

## 4. 启动入口

前台启动并保持标准输入打开：

```bash
mkdir -p ./data
docker run --rm -i -v "$PWD/data:/data" minivectordb:local
```

- 容器内启动命令：`minivectordb --data-dir /data`；
- 使用 `-i`：测试驱动通过管道向容器 stdin 发送 JSONL 请求；
- 不使用 `-d`：数据库必须以前台方式运行，容器生命周期与数据库进程一致；
- 不使用 `-t`：评测环境不分配 TTY，TTY 会干扰标准输入输出的自动读取；
- 不需要 `EXPOSE`、端口映射或 `--host/--port` 参数。

程序启动完成后先输出一行就绪标识，然后进入请求循环：

```json
{"type":"ready"}
```

stdin 关闭或收到 `{"op":"shutdown"}` 请求时，程序回滚未提交事务、完成必要落盘并退出。

## 5. `/data` 数据目录

所有持久化文件统一写入容器内 `/data`：

```text
/data/
├── catalog.dat     # 表定义目录
├── wal.log         # 预写日志
└── <table>.tbl     # 每张表一个页式数据文件
```

本地运行时通过 `-v "$PWD/data:/data"` 把宿主机目录挂载到 `/data`。同一个数据目录可以跨容器重启复用；重启和恢复测试必须复用原目录，不能删除或更换。每个正式用例从一个独立的空数据目录开始。

## 6. STDIN/STDOUT 协议

协议遵循 `2026-competition-round2` 的 [STDIO_PROTOCOL.md](https://github.com/oceanbase/seekdb/blob/2026-competition-round2/STDIO_PROTOCOL.md)：stdin/stdout 使用 JSON Lines，每条报文占一行。测试驱动按“发送一个请求、等待一个响应”的顺序执行。

### 6.1 请求

每行一个 JSON 对象。执行 SQL：

```json
{"id":1,"op":"execute","sql":"CREATE TABLE docs (id BIGINT PRIMARY KEY, title VARCHAR(64), embedding VECTOR(4));","search_mode":"exact"}
```

字段说明：

| 字段 | 必填 | 说明 |
| --- | --- | --- |
| `id` | 是 | 请求编号；响应会原样返回，用于匹配请求 |
| `op` | 是 | `execute` 或 `shutdown` |
| `sql` | `execute` 时必填 | 单条 SQL 语句，可带结尾分号 |
| `search_mode` | 否 | `exact`（默认）或 `ann`；当前实现两种模式均走精确扫描 |

正常关闭：

```json
{"id":99,"op":"shutdown"}
```

### 6.2 响应

成功且无结果集（DDL/DML/事务语句）：

```json
{"id":1,"ok":true}
```

成功且有结果集（SELECT）：

```json
{"id":3,"ok":true,"columns":["id","title"],"rows":[[1,"Alpha"],[2,"Beta"]]}
```

失败：

```json
{"id":4,"ok":false,"error":{"code":"table_not_found","message":"table not found: docs"}}
```

- 每条响应一行，输出后立即 flush；
- stdout 只承载协议响应，日志和调试信息一律写 stderr；
- 单条 SQL 失败不会终止进程，后续请求仍可执行；
- 错误码包括 `syntax_error`、`table_not_found`、`table_exists`、`column_not_found`、`duplicate_key`、`type_mismatch`、`dimension_mismatch`、`invalid_vector`、`varchar_too_long`、`unsupported_sql`、`transaction_error`、`constraint_violation`、`internal_error`、`io_error`。

> `tests/smoke.sql` 保存便于阅读的 SQL 文本；`tests/run_tests.sh` 会把它转换成上述 JSONL 请求后再通过管道送入程序。

## 7. SQL 支持范围

完整语法、语句边界、结果集格式、错误码和精确/近似模式切换最终以 2026-competition-round2 配套测试驱动和题包为准。当前实现覆盖题面要求的最小集合。

### 7.1 表结构

```sql
CREATE TABLE docs (
  id BIGINT PRIMARY KEY,
  title VARCHAR(64),
  embedding VECTOR(4)
);
```

- 支持 `BIGINT`、`VARCHAR(n)`、`VECTOR(n)`；
- 每表一个 `BIGINT` 主键，最多一个向量列；
- `VARCHAR(n)` 的 `n` 按 Unicode 码点计数；
- 向量为固定维度 `float32` 数组，拒绝 NaN、Infinity、维度不符和转换溢出；
- 可选：`CREATE VECTOR INDEX index_name ON table_name(vector_column);`（当前为元信息占位，不构建独立 ANN 索引）。

### 7.2 数据操作

- `INSERT INTO ... VALUES (...), (...);`：支持多行插入；
- `UPDATE ... SET ... WHERE ...;`：支持按主键或条件更新；
- `DELETE FROM ... WHERE ...;`：支持按主键或条件删除；
- 主键不可重复；
- 未显式 `BEGIN` 时，每条成功的数据修改语句自动提交。

### 7.3 普通查询

- `SELECT *` 或普通列投影；
- 比较运算符：`=`、`<`、`<=`、`>`、`>=`；
- 多个条件用 `AND` 连接；
- `ORDER BY`、`LIMIT`；
- 排序并列时按主键升序；未指定 `ORDER BY` 时按主键升序输出，保证结果确定。

### 7.4 向量检索

- `L2_DISTANCE(vector_column, query_vector)` 返回欧氏距离；
- 支持精确 TopK；
- 支持带普通列过滤的 TopK：先在满足 `WHERE` 的记录集合内过滤，再计算距离取前 K 条；
- 结果不足 K 条时返回全部满足条件的记录；
- 距离并列按主键升序；
- `search_mode` 为 `ann` 时当前仍使用精确扫描，返回结果正确，但暂无近似索引加速。

示例 SQL：

```sql
SELECT id, L2_DISTANCE(embedding, '[0.9, 0.1, 0.0, 0.0]') AS dist
FROM docs
ORDER BY dist
LIMIT 2;
```

### 7.5 事务与持久化

- `BEGIN`、`COMMIT`、`ROLLBACK`；
- 同一时刻最多一个显式事务；
- 事务内可以读到自己的修改，普通查询与向量检索看到一致的记录集合；
- 语句失败不留下部分修改；
- `COMMIT` 成功响应发出前完成必要的 WAL `fsync`；
- 正常关闭和异常重启后，已确认提交的数据不丢，未提交事务不会部分生效。

### 7.6 明确不支持

以下语句/特性返回约定错误，不会静默改写为其他语义：

- `NULL`、`JOIN`、聚合、子查询；
- `ALTER TABLE`、外键；
- 嵌套事务、主键更新；
- 多客户端并发和分布式执行。

## 8. 从空目录开始的完整操作示例

以下示例假设当前目录为空，先创建本地数据目录：

```bash
mkdir -p ./data
```

### 8.1 第一次启动：建表、写入、普通查询、向量检索、回滚与提交

```bash
docker run --rm -i -v "$PWD/data:/data" minivectordb:local <<'JSONL'
{"id":1,"op":"execute","search_mode":"exact","sql":"CREATE TABLE docs (id BIGINT PRIMARY KEY, title VARCHAR(64), embedding VECTOR(4));"}
{"id":2,"op":"execute","search_mode":"exact","sql":"INSERT INTO docs VALUES (1, 'Alpha', '[1.0, 0.0, 0.0, 0.0]'), (2, 'Beta', '[0.8, 0.2, 0.0, 0.0]'), (3, 'Gamma', '[0.0, 1.0, 0.0, 0.0]');"}
{"id":3,"op":"execute","search_mode":"exact","sql":"SELECT id, title FROM docs ORDER BY id;"}
{"id":4,"op":"execute","search_mode":"exact","sql":"SELECT id, title, L2_DISTANCE(embedding, '[1.0, 0.0, 0.0, 0.0]') AS dist FROM docs ORDER BY dist LIMIT 2;"}
{"id":5,"op":"execute","search_mode":"exact","sql":"SELECT id, title, L2_DISTANCE(embedding, '[1.0, 0.0, 0.0, 0.0]') AS dist FROM docs WHERE title = 'Alpha' ORDER BY dist LIMIT 2;"}
{"id":6,"op":"execute","search_mode":"exact","sql":"BEGIN;"}
{"id":7,"op":"execute","search_mode":"exact","sql":"INSERT INTO docs VALUES (4, 'RollbackMe', '[0.0, 0.0, 1.0, 0.0]');"}
{"id":8,"op":"execute","search_mode":"exact","sql":"SELECT id, title FROM docs ORDER BY id;"}
{"id":9,"op":"execute","search_mode":"exact","sql":"ROLLBACK;"}
{"id":10,"op":"execute","search_mode":"exact","sql":"SELECT id, title FROM docs ORDER BY id;"}
{"id":11,"op":"execute","search_mode":"exact","sql":"BEGIN;"}
{"id":12,"op":"execute","search_mode":"exact","sql":"INSERT INTO docs VALUES (5, 'Committed', '[0.0, 0.0, 0.0, 1.0]');"}
{"id":13,"op":"execute","search_mode":"exact","sql":"COMMIT;"}
{"id":14,"op":"execute","search_mode":"exact","sql":"SELECT id, title FROM docs ORDER BY id;"}
{"id":15,"op":"shutdown"}
JSONL
```

### 8.2 第二次启动：验证重启后的持久化结果

```bash
docker run --rm -i -v "$PWD/data:/data" minivectordb:local <<'JSONL'
{"id":101,"op":"execute","search_mode":"exact","sql":"SELECT id, title FROM docs ORDER BY id;"}
{"id":102,"op":"execute","search_mode":"exact","sql":"SELECT id, L2_DISTANCE(embedding, '[0.0, 0.0, 0.0, 1.0]') AS dist FROM docs ORDER BY dist LIMIT 2;"}
{"id":103,"op":"shutdown"}
JSONL
```

预期行为：

- `RollbackMe` 在 `ROLLBACK` 后不可见；
- `Committed` 在 `COMMIT` 后可见；
- 第二次启动复用同一 `/data` 后仍能查询到 `Committed`，说明提交已持久化；
- 向量查询按距离升序返回，距离相同时按主键升序；
- `SELECT` 响应的 `columns` 为列名数组，`rows` 为按行组织的 JSON 数组。

## 9. 本地编译与冒烟测试

先编译，再运行测试：

```bash
make -j4
chmod +x tests/run_tests.sh
bash tests/run_tests.sh
```

脚本行为：

1. 检查仓库根目录是否存在可执行的 `./minivectordb`；不存在时提示先执行 `make`，退出码为 2；
2. 清理并创建临时数据目录 `/tmp/mvdb_test_data`；
3. 读取 `tests/smoke.sql`，将每条 SQL 封装为 `execute` 请求，通过管道喂给程序；
4. 检查 `ready`、每条响应 `ok`、事务回滚/提交后的记录集合、向量检索顺序；
5. 关闭进程并复用 `/tmp/mvdb_test_data` 再次启动，验证重启后的持久化结果。

脚本失败时会打印原始响应和 stderr，便于定位。构建镜像后，可用第 4 节的启动方式和第 8 节的 JSONL 请求格式进行容器内验证：

```bash
docker build --platform linux/amd64 -t minivectordb:local .
docker run --rm -i -v "$PWD/data:/data" minivectordb:local
```

当前程序不直接读取裸 SQL 文件；`tests/smoke.sql` 必须先由 `tests/run_tests.sh` 转换为 JSONL 请求。

仓库另附 `tests/run_cases.py`（官方公开用例）与 `tests/bench.py`（10,000 行 × 128 维性能基准），使用方法见 `tests/official/README.md`；这些结果用于本地诊断，正式成绩以平台评分为准。

## 10. 已知限制与设计说明

详细的记录布局、向量索引方案、事务处理、提交持久化与恢复流程见 [`docs/DESIGN.md`](docs/DESIGN.md)。当前已知的主要限制：

- `CREATE VECTOR INDEX` 尚未构建独立 ANN 索引，`ann` 模式退化为精确扫描；
- 主键定位索引和扫描时的主键列表常驻内存，超大数据集需要分块设计；
- 单进程单事务，不支持并发连接；
- 崩溃模型为进程强制终止，不模拟断电和磁盘损坏。

题目只要求单机、单进程、单客户端连接和最多一个显式事务，不要求并发控制、分布式执行或完整 SQL 标准。

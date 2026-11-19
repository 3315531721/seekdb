-- MiniVectorDB 冒烟测试 SQL 样例
-- 本文件是便于阅读的 SQL 文本；tests/run_tests.sh 会逐行转换为 JSONL 协议请求。
-- 约定：每条 SQL 占一行，以分号结尾；空行和 -- 开头的注释会被忽略。

-- 1. 建表
CREATE TABLE docs (id BIGINT PRIMARY KEY, title VARCHAR(64), embedding VECTOR(4));

-- 2. 多行插入
INSERT INTO docs VALUES (1, 'Alpha', '[1.0, 0.0, 0.0, 0.0]'), (2, 'Beta', '[0.8, 0.2, 0.0, 0.0]'), (3, 'Gamma', '[0.0, 1.0, 0.0, 0.0]');

-- 3. 普通查询（SELECT *）
SELECT * FROM docs ORDER BY id;

-- 4. 精确向量 TopK
SELECT id, title, L2_DISTANCE(embedding, '[1.0, 0.0, 0.0, 0.0]') AS dist FROM docs ORDER BY dist LIMIT 2;

-- 5. 带普通列过滤的向量检索
SELECT id, title, L2_DISTANCE(embedding, '[1.0, 0.0, 0.0, 0.0]') AS dist FROM docs WHERE title = 'Alpha' ORDER BY dist LIMIT 2;

-- 6. 更新
UPDATE docs SET title = 'BetaUpdated' WHERE id = 2;

-- 7. 验证更新
SELECT id, title FROM docs ORDER BY id;

-- 8. 删除
DELETE FROM docs WHERE id = 3;

-- 9. 验证删除
SELECT id, title FROM docs ORDER BY id;

-- 10. 开启事务
BEGIN;

-- 11. 事务内插入（稍后回滚）
INSERT INTO docs VALUES (4, 'RollbackMe', '[0.0, 0.0, 1.0, 0.0]');

-- 12. 事务内应能读到自己的写入
SELECT id, title FROM docs ORDER BY id;

-- 13. 回滚
ROLLBACK;

-- 14. 回滚后 RollbackMe 不可见
SELECT id, title FROM docs ORDER BY id;

-- 15. 再开一个事务
BEGIN;

-- 16. 事务内插入（稍后提交）
INSERT INTO docs VALUES (5, 'Committed', '[0.0, 0.0, 0.0, 1.0]');

-- 17. 事务内更新
UPDATE docs SET title = 'BetaFinal' WHERE id = 2;

-- 18. 提交
COMMIT;

-- 19. 提交后可见
SELECT id, title FROM docs ORDER BY id;

-- 20. 提交后的向量检索
SELECT id, title, L2_DISTANCE(embedding, '[1.0, 0.0, 0.0, 0.0]') AS dist FROM docs ORDER BY dist LIMIT 2;

#!/bin/bash

# 设置环境
cd "$(dirname "$0")/.."
TEST_DIR="test_run"
LOG_DIR="$TEST_DIR/logs"
mkdir -p "$LOG_DIR"

echo "=== FlashOrder 集成测试 ==="
echo "测试目录: $TEST_DIR"
echo "日志目录: $LOG_DIR"

# 清理之前的进程
echo "清理之前的进程..."
pkill -f hotstuff-app 2>/dev/null || true
sleep 1

# 启动单个副本
echo "启动副本 0..."
./build/examples/hotstuff-app --conf "$TEST_DIR/test.conf" --idx 0 > "$LOG_DIR/replica0.log" 2>&1 &
REPLICA_PID=$!

echo "副本 0 PID: $REPLICA_PID"
sleep 3

# 检查副本是否在运行
if ps -p $REPLICA_PID > /dev/null; then
    echo "✓ 副本 0 启动成功"
else
    echo "✗ 副本 0 启动失败"
    cat "$LOG_DIR/replica0.log"
    exit 1
fi

# 等待几秒让副本完全启动
echo "等待副本初始化..."
sleep 5

# 启动客户端进行简单测试
echo "启动客户端测试..."
./build/examples/hotstuff-client --idx 0 --iter 5 --max-async 5 > "$LOG_DIR/client.log" 2>&1 &
CLIENT_PID=$!

echo "客户端 PID: $CLIENT_PID"
sleep 10

# 检查客户端是否完成
if wait $CLIENT_PID 2>/dev/null; then
    echo "✓ 客户端测试完成"
else
    echo "✗ 客户端测试失败"
fi

# 停止副本
echo "停止副本..."
kill $REPLICA_PID 2>/dev/null || true
wait $REPLICA_PID 2>/dev/null || true

# 检查日志中的关键信息
echo "=== 检查日志 ==="
echo "副本日志中的 FlashOrder 相关消息:"
grep -i "flashorder\|hypergraph\|finalize" "$LOG_DIR/replica0.log" | head -10

echo "客户端日志摘要:"
tail -20 "$LOG_DIR/client.log"

echo "=== 测试完成 ==="
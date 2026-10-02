#!/bin/bash

echo "=== FlashOrder 集成诊断测试 ==="
echo "1. 检查编译配置..."
grep "HOTSTUFF_ENABLE_BENCHMARK" build/include/hotstuff/config.h

echo -e "\n2. 检查二进制文件..."
ls -lh build/examples/hotstuff-app build/examples/hotstuff-client

echo -e "\n3. 生成新的密钥对..."
cd build && ./hotstuff-keygen > /tmp/keypair.txt 2>&1
echo "生成的密钥对:"
cat /tmp/keypair.txt

echo -e "\n4. 创建简单的测试配置..."
cat > /tmp/test_simple.conf << EOF
block-size = 10
nworker = 2
repnworker = 2
clinworker = 2
repburst = 100
cliburst = 100
sb-users = 10
sb-prob-choose_mtx = 0.5
sb-skew-factor = 0.5
fairness-parameter = 1.0
pace-maker = rr
replica = 127.0.0.1:11000;21000, 032eeaba58859dde487722e36af0ab1bd424fa7682437b53fa5d4f16928b526963, 8676ab2fd00b57884361ebf619a4b810edc7f470e96a4b583936db8a4efbc59f
EOF

echo "测试配置已创建到 /tmp/test_simple.conf"

echo -e "\n5. 尝试启动单个副本（5秒测试）..."
timeout 5 ./build/examples/hotstuff-app --conf /tmp/test_simple.conf --idx 0 --notls 2>&1 | head -30

echo -e "\n6. 检查客户端参数..."
./build/examples/hotstuff-client --help 2>&1 | grep -E "idx|iter|max-async" | head -5

echo -e "\n=== 诊断完成 ==="
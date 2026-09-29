# 2Box CPU 自动分配 V2

## 为什么 V1 会导致卡死/单核 100%

V1 在 MemoryDll 的 `CreateProcessA/W` Hook 中给每一个被创建的进程调用 `SetProcessAffinityMask`，同时 CPU 分区数量使用全部 Environment 数量。2Box 的 Environment 可以是历史/空环境，因此可能出现“实际只运行一个程序，但 CPU 被限制到一个逻辑核心”的情况。另一个风险是把目标程序创建的辅助子进程也强制限制到同一个 CPU 集合。

## V2 设计

CPU Affinity 不再通过 MemoryDll 的 CreateProcessW Hook 设置。目标进程完成 2Box RPC login 后，由 2Box 主进程统一给“正在运行的 Environment”均分逻辑 CPU。

这样做有三个好处：

1. 空的/历史 Environment 不占 CPU 配额。
2. 不会把目标程序创建的所有子进程强制限制在同一个 CPU 集合；Windows 会让子进程继承父进程的 Affinity。
3. Affinity 设置发生在目标程序正式 Resume 前的初始化阶段，减少启动过程中改变调度环境导致的兼容性风险。

## 分配规则

只统计 `getAllProcessesCount() > 0` 的 Environment。某个 Environment 第一次登录 2Box 时重新均分所有活跃 Environment。

例如 16 个逻辑 CPU、2 个活跃 Environment：

- Env 1 -> CPU 0-7
- Env 2 -> CPU 8-15

如果当前只有一个活跃 Environment，它会获得全部 CPU，不会出现“单实例被限制到一个核心”的问题。

## 当前限制

当前实现仍针对单 Processor Group、最多 64 个逻辑 CPU。超过 64 逻辑 CPU 的机器后续应升级到 Processor Groups / CPU Sets。

## 建议测试

1. 删除/关闭旧的 2Box 实例后重新启动。
2. 只启动一个目标程序，确认它可以使用全部逻辑 CPU。
3. 启动第二个 Environment，确认两个 Environment 被分成两组 CPU。
4. 观察子进程是否正常启动。
5. 再启动第三、第四个 Environment，确认每组 CPU 随活跃 Environment 数量重新均分。

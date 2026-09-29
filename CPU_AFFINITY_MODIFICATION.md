# 2Box CPU 自动分配修改版

本版本保持原有 2Box/2Box-cli 工作方式不变，新增自动 CPU Affinity 分配。

## 分配逻辑

假设 Windows 在处理器组 0 中有 N 个逻辑处理器，当前有 M 个 2Box 环境：

- M=1：该环境使用全部 N 个逻辑处理器。
- M=2：平均分成两组，例如 16 线程 -> CPU 0-7 / 8-15。
- M=3：平均分成三组，例如 16 线程 -> CPU 0-4 / 5-9 / 10-15。
- M=4：平均分成四组，例如 16 线程 -> CPU 0-3 / 4-7 / 8-11 / 12-15。
- M>N：多个环境按逻辑处理器轮流共享。

当创建或删除环境时，2Box 会重新计算所有环境的 CPU 范围，并立即对已经运行的进程调用 `SetProcessAffinityMask`。

新创建的目标进程还会在 `MemoryDll` 的 `CreateProcessW/CreateProcessA` Hook 中，在恢复线程之前设置相同的 CPU Affinity，因此目标程序后续创建的子进程也会继承该限制。

## 修改位置

1. `2Box/biz/env/Env-EnvManager.*`
   - 根据当前环境数量计算 CPU 分区。
   - 创建/删除环境时重新平衡已有进程。
2. `2Box/biz/launcher/Launcher.cpp`
   - 把计算出的 CPU mask 写入 Detours payload。
3. `common/header_units/sys_defs.h`
   - 给 `DetourInjectParams` 增加固定 64 位 CPU mask 字段。
4. `MemoryDll/global_data/*`
   - 接收并保存 CPU mask。
5. `MemoryDll/hook/Hook-Kernel32.ixx`
   - 目标进程创建成功、注入完成后、ResumeThread 前调用 `SetProcessAffinityMask`。

## 注意

当前实现针对普通 Windows 桌面环境（逻辑处理器 <= 64，全部位于 processor group 0）设计。

如果 CPU 超过 64 个逻辑处理器，建议进一步改成 Windows Processor Group / CPU Sets 方案。

CPU Affinity 设置失败不会阻止程序启动；这项功能属于性能调度优化，不改变 2Box 原有的多实例隔离逻辑。

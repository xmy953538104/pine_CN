# 路线 B 的证据层级与本轮终点

基线为 `55b3fab139e0891d3278715406ed1f843686b687`。本轮交付独立 Android/Bionic 一次运行工具；不安装 Pine APK，不打开生产 Gate，不改普通 Loader、调度、Guest RAM、NVN、Vulkan 或游戏文件。Windows 工具只允许自己的随机 ADB 目录。设备未回传前状态为 **ANDROID_EXECUTION_PENDING**。

## 决策

**只继续本次独立 Bionic 边界验收；暂停真实 NCE 路线 B patch 的部署推进。下一条可收敛的原生取证路线是既有终止信号 ucontext 的 POD 快照，不继续靠增加同类合成数量等待 Gate 自动成立。**

这不表示调用前原始 X0 需要 Horizon GOT 最终目标证明，也不表示只有全局重构 RWX 才存在解法。目标区域的全部可写别名和写者若能受控，局部协议可以足够。但当前 Pine 无法兑现该协议；现有边界又不允许本轮改变相应写路径/映射操作。不能以接口名字、计数器、一次哈希、部分 memcpy 通知或 `active=0` 代替强制保护。

## 分层证据契约

| 层级 | 可以声称的事实 | 本层必要条件 | 本层不需要/不能声称 |
| --- | --- | --- | --- |
| L0 加载身份 | 原始 NSO Build ID、未填充段哈希，以及独立的 post-NCE / post-observer 窗口副本 | 加载拥有者、布局/范围、精确字节匹配、缓存发布与代际 | 不证明运行期持续有效；不需要最终 GOT |
| L1 原始调用点快照 | 固定 sdk+0x2D64E0 的 BL 后到达 stub，捕获 X0、硬件 LR=sdk+0x2D64E4、原始 SP；标记 UNCLASSIFIED | 当前 callsite 4B、stub 208B、原 PLT 16B 的执行身份/映射生命周期；ThreadContext/槽代际；X16/X17 与原 PLT 覆盖匹配；原 flags/SIMD/栈语义 | 不需要最终 GOT/Resolver 目标或 AbortImpl 入口证明；记录不能证明捕获后已经进入 PLT |
| L2 helper 失败分支分类 | NO_CALLBACK / INSUFFICIENT_AFTER_RETURN 的调用者/断言来源 | L1 加调用者分支窗口及相关断言内容/地址的当前身份；分类证据闭合 | 不能仅凭加载时字符串地址或 QEMU 自有内容替真实游戏分类；仍不天然要求最终 GOT |
| L3 PLT/Resolver 的实际最终目标 | 某次解析/跳转实际绑定到哪个目标 | 真正 Horizon rtld/GOT 更新协议及当次绑定见证 | GNU fixture resolver、静态 PLT 反汇编不是 Horizon 证据 |
| L4 真正进入 AbortImpl | 实际到达该函数入口的原始状态 | 独立入口见证或足够的实际执行证据及该入口身份 | L1 的调用前 X0/硬件 LR 不等于外层 Abort/AbortImpl 入口 |

原始 BL 字节仍是 `c04c0494`（word `0x94044cc0`），原 PLT 为 sdk+0x3E97E0。安装器只重定向 BL，保留原硬件 LR 并尾跳原 PLT，不读写 GOT。参见 [observation.cpp](../app/src/main/cpp/skyline/nce/observation.cpp) 和 [observation_stub.S](../app/src/main/cpp/skyline/nce/observation_stub.S)。`original_plt=traversal_preserved_target_not_read` 描述保留的路径，不证明捕获后的下一条路径已经完成。

因此，**无法证明最终 GOT 不是阻止 L1 原始快照的共同理由**。当前真正阻止 L1 部署的是下面的 RWX/别名/映射执行生命周期，以及尚未完成的真实 Android/Pine 边界验收。L2/L3/L4 分别保留自己的更强条件，不把它们强加给 L1。

## 可从当前源码证明的缺口

| 当前路径 | 源码证据 | 保护缺口 |
| --- | --- | --- |
| Guest CPU 直接写 | [memory.cpp:122](../app/src/main/cpp/skyline/kernel/memory.cpp#L122) 的实际映射权限为 RWX | Guest store 不经过 MemoryManager 通知；4B callsite / 208B stub / 16B PLT 同样可被改写 |
| Host 裸指针 | [GetHostSpan:683](../app/src/main/cpp/skyline/kernel/memory.cpp#L683) 直接返回可写 span | 没有借用期、写者登记、写 epoch 或撤销确认；原有持有者以后写入不可见 |
| 缓存别名 | [CreateMirror:386–404](../app/src/main/cpp/skyline/kernel/memory.cpp#L386) 以 mremap 建长期 RW mirror；CreateMirrors 同类 | 创建时撤销不约束以后写；仅保护主 Guest 映射不能封闭别名 |
| GPU/Host 复制 | [buffer.cpp](../app/src/main/cpp/skyline/gpu/buffer.cpp) Write/Copy/Synchronize 中 memcpy 到 mirror；[buffer_manager.cpp](../app/src/main/cpp/skyline/gpu/buffer_manager.cpp) 合并复制 | 写入不统一走观察者通知；不能未经证明把所有长期指针列为受控 |
| loader/NCE patch | [loader.cpp:96–106](../app/src/main/cpp/skyline/loader/loader.cpp#L96) 调用 PatchCode 后直接复制文本/RO/data | 当前加载期独占校验有用，但不是以后写者的强制协议；本轮不改变普通 Loader |
| 卸载/复用 | [FreeMemory:591–600](../app/src/main/cpp/skyline/kernel/memory.cpp#L591) 通知后直接 MADV_REMOVE；MapInternal 通知后 mprotect | 不等待已进入 stub 的线程退出，也不保护尾跳 PLT 所需映射；Module shared_ptr 只保元数据 |
| 现有 trap | [trap_manager.cpp](../app/src/main/cpp/skyline/common/trap_manager.cpp) HandleTrap 可把保护降为 None 并重新放开 RWX，且有锁/回调 | 当前 GPU trap 不是代码窗口完整性租约；不能直接复用它宣称跨全部 alias 的持续保护 |
| rtld/GOT | [nso_image.h](../app/src/main/cpp/skyline/diagnostics/nso_image.h) LiveGotCapability 返回 ResolverProtocolUnavailable | L3 尚无真正 resolver 写协议；这是 L3 缺口，不是 L1 必要条件 |

BeforeMappingMutation 现在包含 text、RO 和 patch reservation，两处 SVC memcpy 也提前撤销。它们是有价值的已知路径通知，仍不封闭上表裸写或在途使用。`active=0` 只能停止新记录提交，已经进入 stub 的线程仍可能继续读桩、尾跳原 PLT。周期 hash 存在检查间隙，不能阻止修改/复用，因此不是保护。

## 最小局部协议：接口及其兑现范围

以下是**尚未实现的契约**，不是已有能力或已通过的安全条件：

1. **受观测区域准入**：取得 mapping generation，登记 L1 的精确三段范围及页边界，证明其全部可写 alias/borrowed pointer/writer 受控；不能证明则不安装。L2 才额外加入调用者/断言 RO。不是扩大到整个 NVN/GPU 或全部 Guest RAM。
2. **执行租约**：在进入相关 Guest 执行前建立不可失效的映射拥有权，在确知相关原生执行者不再引用 stub/PLT 后释放；阻止卸载、权限变化和地址复用。记录槽/Module shared_ptr 不等于该租约。
3. **撤销后 mutation**：先关闭该 generation 的准入，撤销记录有效性；旧执行者确认退出后才允许写、mprotect、MADV_REMOVE 或复用。不能持有 VM mutex 等待可能正在进入 SVC/VM 路径的线程；需要分阶段撤销、放锁、静默确认、重新取锁验证 generation。

兑现第1项至少涉及目标页所有 alias 的登记/权限或写路径约束，包含 Guest CPU store；第2/3项涉及 MemoryManager mutation 与真实线程执行边界。不需要预先改变全局 NVN/Vulkan 策略，但也绝非仅加 observer 计数器或更多回归能完成。当前不具备所有 alias 的受控准入证据，且这些目标映射/写操作变化不在本轮授权范围，故选择暂停真实 patch 部署。将来只有 Owner 明确接受这一个有限局部协议的行为影响，才值得另立实现与 Android 安全验收；否则路线 B 在当前边界下到此为止。

初始 NCE TLS 发布窗口另未闭合：[KThread::Run](../app/src/main/cpp/skyline/kernel/types/KThread.cpp) 先写 Guest TPIDR_EL0，再保存 hostTpidrEl0/hostSp，信号可能读到未发布的 native TLS。最小修复范围是先发布两个 native 字段再切换 Guest TLS，或提供等价的可证明 entry protocol；必须逐边界测试真实 ARM64/Android/ART 异步递送。null-hostSp 单测与等到 spin-entry 后才发信号均不是该证明。本轮不改普通 Run/调度。

## 具体替代：原生终止现场，而非新可执行 patch

下一步可以限定为既有原生终止信号路径：普通线程初始化时预分配一个生命周期拥有的 POD 槽；在真实信号 ucontext 中仅复制 PC/X0/LR/SP、信号号和 owner generation，用 release 发布；沿原有退出流程回到普通上下文后再序列化/导出，线程 join 后再回收。故障现场不新增分配、IO、JNI 或等待，不安装新的 Guest BL/stub，也不要求 GOT 最终目标证明。

该记录必须标为**终止信号现场**，先保持 UNCLASSIFIED。终止时 X0/LR 可能已与调用前值不同，不能倒推为 sdk+0x2D64E0 的 X0，也不能自动称已进入 AbortImpl。已有非 SIGINT NCE handler 仍有格式化/日志/Process::Kill/JVM，真正的最小 POD 入口要放在其前并独立审查；TLS 初始窗口、ThreadContext 所有权和真正 Android/ART 信号递送仍须解决，但不再引入一套无法封闭 RWX 的新 Guest patch。这是有限、可审查的下一轮取证目标，本轮不顺带实现整套系统。

## 本轮验收终点与回退

交付 Windows 一次运行包、正式 NDK ARM64/Bionic 程序与匹配 libc++、结构化失败/未知收集，以及本文决策。没有已授权 Android 设备的有效运行结果时，一律 ANDROID_EXECUTION_PENDING；不以 NDK、Linux/QEMU、假 ADB 传输测试或既有14组数量替代。

保留 PR22 的 APK/符号与回退、8e659d8a 已交付 APK、PR23/24、LoggerHistory 基线和 main。本轮不制作另一个不能捕获真实 NVN 的 Pine APK，不要求 Owner 再进入 Hamlet。生产 `observer_installation=disabled`、`runtime_connected=false`、真实游戏记录0。

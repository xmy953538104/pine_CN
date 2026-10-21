# Pine 独立 Android/Bionic 一次验证包

本包初始状态为 **ANDROID_EXECUTION_PENDING**。NDK 编译、Linux/QEMU 和 Windows 收集器测试均不能把它改成 Android 实测。只有设备回传的原生 JSON、退出码、源码提交和产物哈希全部匹配，才会报告 `ANDROID_BIONIC_EXECUTED_PASS`。

这是独立程序的线程与信号测试，包含自有 guard page 的 SIGSEGV 和自己线程的 SIGINT。不是游戏崩溃复现；不启动 Pine，不读写 Pine、NSP、存档或其他应用目录，不需要 Root，不改系统/网络/权限设置，不读取 logcat，不使用 SIGTRAP。Pine 生产观测 Gate 始终 CLOSED。

## Windows 准备

1. 完整解压 ZIP 到普通可写目录，保留 `host`、`device`、`manifest.json` 等相对位置。不要在 ZIP 预览窗口直接运行。
2. 如果没有现成 ADB，从 [Google 官方 Platform-Tools](https://developer.android.com/tools/releases/platform-tools) 下载 Windows ZIP，解压后把整个 `platform-tools` 文件夹放到本包根目录。也可使用 PATH 中现有的官方 ADB，或在 PowerShell 入口指定 `-AdbPath`。无需安装 Node、Python、NDK 或 APK；Windows x64 的 Node 运行时已随包提供并核验官方 SHA256。
3. 使用已经获得设备授权的 USB/无线 ADB 连接。若设备仍为 `unauthorized`，收集器会保存 PENDING 结果后停止；只有 Owner 可以在设备上处理授权，不会自动点击、配对或重试。不要在设备未授权时反复启动测试。
4. 设备须为 Android API29+、`arm64-v8a`、`aarch64` 内核。仅查询这三项系统属性，不收集私人资料。

## 执行一次

双击 **Start-PineBionic.cmd**。这是原生 Windows 运行时入口，不改 PowerShell ExecutionPolicy。一个设备时自动选择唯一设备；多个设备时会列出序号，必须由 Owner 明确选择，不能静默选第一台。

也可以在正常允许脚本执行的 PowerShell 中运行：

```powershell
.\Run-PineBionic.ps1 -AdbPath 'C:\Android\platform-tools\adb.exe' -Serial '设备序列号'
```

ADB 路径和输出位置可含空格。只有多设备/指定连接才需要 `-Serial`，指定输出父目录可加 `-OutputDirectory 'D:\Pine test results'`。脚本仅在其中创建新的随机子目录，不覆盖旧结果。若组织策略阻止 PowerShell 脚本，不要修改策略或使用 Bypass；使用上面的 CMD 原生入口，或交由组织批准程序运行。

收集器先验证本地产物哈希、设备与授权，再创建唯一目录 `/data/local/tmp/pine-bionic-<32位随机ID>`。它只在该目录 push、校验、chmod 自身三个程序、执行一次、pull；不执行 adb root、重启、改设置、启动游戏、删除数据或全局日志采集。

每个测试子程序最多运行一次。原生父进程给每个子进程 90 秒自限时，首个失败即停止后续子程序。不会自动重跑崩溃。ADB 传输超时时，状态为 UNCERTAIN；不会把未知当成功，也不会再次执行同一测试。保留远端目录，便于只读收集，而不是再启动程序。

## 回传结果

窗口末尾会显示完整 `result.json` 路径和原始退出码。把该文件所在的整个 `results\pine-bionic-*` 随机子目录压缩后回传即可。它包含：

* `result.json`：设备/源码/哈希校验、唯一 run ID、ADB 与原生退出码、是否确实启动过、收集成功/失败及最终状态。
* `device\android-result.json`：设备原生父进程的结果、各子进程退出码/信号和是否达到限时。
* `device\lifecycle-logs`：真实 pthread/Join、setjmp/longjmp、异常路径、sigaltstack/嵌套信号、真正 Writing 中断、槽/Module 最后所有权的结构化检查和自有 writer 记录。
* `device\register-logs`：复用既有 fixture 的寄存器/flags/SIMD、Guest/Host SP/TLS、硬件 LR、原 PLT 和原函数不返回的检查。
* 各子程序自己的 stdout/stderr。失败时仍尝试 pull 这个唯一目录；没有完整 JSON 或断连会明确标为未知，不会显示通过。

若失败，请保留证据并回传，**不要再次运行来复现崩溃**。若失败发生在授权/ABI/哈希检查前，没有启动原生程序，JSON 会写 `native_execution_attempts=0`。若传输失败，也请回传已有本地目录，其中保留准确动作和错误。

## 通过能说明什么

通过只能说明这个独立、受控程序在该 Android/Bionic 环境完成了所列边界。生产 KThread/KNce/信号/加载片段按源码提取；Pine scheduler、VFS、JVM、真实映射管理与 Horizon resolver 仍是受控依赖。新 Writing 用例实际执行原样桩，guard page 在 Writing/X0/LR 写入后、SP 写入时触发，再由异步 SIGINT 退出；不是预填 Writing。故障处理里的确定性暂停只属于测试依赖，不能当成生产故障协议。

这不是 Pine APK/ART sigchain 的完整验收，也不证明 RWX 持续身份、映射租约、初始 TLS 发布窗口或 Horizon rtld。`NO_CALLBACK` / `INSUFFICIENT_AFTER_RETURN` 的真实游戏捕获保持禁用，真实游戏记录为零。不要以本包通过解除生产 Gate。

`manifest.json` 给出精确源码 commit、编译命令/工具版本、每个文件 SHA256；`corresponding-source.tar` 与许可证随包。保留已经交付的 8e659d8a APK 及 PR22 回退，本包不会安装或覆盖它们。

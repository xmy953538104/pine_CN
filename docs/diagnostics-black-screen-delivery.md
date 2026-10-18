# 黑屏诊断 / DFX1 候选交付

应用源码固定为 `79a4d1a2250585ec64233befab934f0e5ed27ab7`，
[应用草稿 PR #4](https://github.com/xmy953538104/pine_CN/pull/4)。
独立 [工具草稿 PR #5](https://github.com/xmy953538104/pine_CN/pull/5) 的文档/observer
提交不重建或重签该 APK；其 observer SHA 单独保存在输入资格证据中。

## 下载及身份

- [O2 Debug APK](https://github.com/xmy953538104/pine_CN/actions/runs/39101206438/artifacts/12127120527)
- [匹配的 native 符号](https://github.com/xmy953538104/pine_CN/actions/runs/39101206438/artifacts/12127215348)
- [O2 构建 CI：成功](https://github.com/xmy953538104/pine_CN/actions/runs/39101206438)
- [126 项 JVM 等五套快检：成功](https://github.com/xmy953538104/pine_CN/actions/runs/39100903782)

| 对象 | 身份 |
|---|---|
| APK 本体 SHA256 | `006ca1cf669a69c0666aa9f148c76e4e01565eb8ba15e2c32d3ce8b279dd5a48` |
| APK artifact ZIP SHA256 | `77a3de63ff3a0f26c07e2ca2117feb3ca741e30b5afe5658e249dc813592ae60` |
| native Build ID | `8b640becc3b39454305b896c2cd4384086544dec` |
| APK 内 libskyline.so SHA256 | `ffa961725c01fc4fd9726b2b86b00f7c251b76901768601cce7bf0f86dd9e5f9` |
| unstripped libskyline.so SHA256 | `74343e9b30197443026ce33d47d9a0a6e24a654676bab6370bae320d30cdbc1e` |
| 符号 artifact ZIP SHA256 | `0fab02bd38b2734a22666420fbcf6a81d01d9bd22f9764304b6f646c385ddea1` |
| 现有 CI 证书 SHA256 | `70edcdc802bb16afe9665ecd3d342ea74b217a79b6efb4dbdfbb3ea3eddf16c1` |

构建实际验证了 ARM64-only/minSDK29/DEX 源码、O2 与保护/断言标志、
Debug 签名、Release 独立 emulation-process manifest，以及 Build ID/DWARF
匹配。JVM 126 项、15 类、0 failure/error/skip；先独立测试，随后才取 native
依赖、加载签名并 assemble。没有正式 Release、合并 main 或商业资源修改。

## 本轮行为及检查

真实 present API 结果及 wrapper 抛错、5 秒低频计数、菜单退出前请求/最后状态、
JNI 实际结束原因、旧 Session critical 状态保留、Base 与 effective ExeFS 来源
均有实现与回归。队列首次分配失败、发布/Flush 竞争、Stop/Start 旧 producer、
实际 pinned Vulkan wrapper 的错误传播、本地 native ASan/UBSan、内容/存储测试
已检查。Windows PowerShell5.1/pwsh 的假 ADB、UTF-8、分组及真实 console
Ctrl+C 测试通过。它们不是 Android 游戏运行证据。

DFX1 仅解析真实 guest 输出：严格 legacy token 兼容、版本/字段/长度/频率限制、
原始与派生消息、脚本字段来源、组合脱敏和离线原文逐项核验均有测试。
[游戏侧最小协议/探针契约](diagnostics-dfx1-protocol.md) 可交给另一开发线；
`TheSim:LuaPrint` 等接口仍待唯一探针校准，未声称 Lua 桥已通。

CI 只显式触发了一份当前 O2 候选。普通提交先跑并行快检；O0/runtime 按需。
无关标签没有取消正在运行的候选。快检 JVM 实测41任务中16项来自 cache；
独立候选 job 首次 Gradle user-home cache 未命中，JVM为2m14s，随后 assemble
为5m34s（含22执行/28 up-to-date/1 cache）；ccache 为816/1034，78.92%命中。
这些是本次实测，不承诺每次构建相同耗时。

## 未运行及复现

**这个新 APK 尚未在 Android 安装或启动。** 已知 KVM 权限限制未绕过或盲目重试。
Owner 对908c/O2速度恢复的确认属于先前源码；不能继承为这个包的运行通过。
真机启动、实际菜单退出/重开/SAF、游戏速度和真实游戏 DFX1 桥仍待验收。
独立已有 APK 输入资格即使成功，也只核验原字节/来源/签名，不代表执行成功。

按 [Owner 检查步骤](diagnostics-black-screen-candidate.md) 使用已有合法内容，
无需重新制作 CleanPatch 或修改游戏。配对后的 USB/无线 ADB 本地采集：

```powershell
.\tools\pine-diag.ps1 -ListDevices
.\tools\pine-diag.ps1 -Serial YOUR_DEVICE -IncludeCrashLog -OutputDirectory .\pine-diag
```

当前样本黑屏后手动退出不被标为自动崩溃；历史 exit 关联 unknown 仍保持 unknown。
present 不是可见像素/菜单/Lua 证据，无 DFX 也不能推出 main.lua 未执行。
未上传私人运行日志、密钥、tickets、ROM 或存档。

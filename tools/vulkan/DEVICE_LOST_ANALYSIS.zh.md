# hdc 设备 GPU DEVICE_LOST 分析与本地布局跟踪修复

日期：2026-11-01。范围：只读读取设备证据，修复源码并完成本地验证；随后按用户要求分块提交，只发布到用户 fork。设备安装与实测由用户后续执行。

## 1. 结论与证据强度

1. **10 份游戏崩溃报告的首因相同：Render Frame 中出现 GpuDeviceLossException / VK_ERROR_DEVICE_LOST。** Java 栈在 VulkanCommandEncoder.awaitSubmitCompletion，并不等于这个等待函数制造了错误。
2. **原始系统日志显示实际 GPU reset 和 page fault。** HM_GPU 先输出 Msync 错误、等待 fence 时已发生 reset、恢复后返回 device lost；随后 shim 的 F69 wait 记录返回错误。不能把它改成成功来“修复”。
3. **本地 shim 有能直接复现的布局跟踪缺陷，且设备确实记录了 layout-not-captured → 原生 push 回退。** 本轮修复布局记录驱逐、生命周期和查询路由问题，并增加宿主回归。它是有证据支持的局部修复，不是仅降低负载或关掉错误日志。
4. **尚未证明 GPU page fault 的唯一触发点，也未声称设备崩溃已根治。** 另有 descriptor layout 兼容性、timeline/fence、partial push 等语义缺口，详见第 6 节。它们需要独立的资源状态测试，不能只修改一处 flag 后假定正确。

设备为 QXS-W10 / Maleoon 916B，驱动报告 Vulkan 1.2.309 B312；游戏为 Minecraft 26.3，LWJGL 3.4.3-snapshot。崩溃记录覆盖 3120×1843、3120×2080 等窗口大小，并非全部固定为同一个尺寸。

## 2. 本轮读取的证据

### 本地证据索引（不随 Git 发布）

下面仅记录排查机上的相对路径，避免远端文档链接到不存在的私有日志；这些文件不是构建或执行回归的前提，也不上传原始系统日志与游戏内容。

~~~text
E1  .test/device-logs/crash-reports/crash-2026-10-29_21.40.35-client.txt
E2  .test/device-logs/logs/latest.log
E3  .test/device-logs/hilog.396.20261029-110156.gz
E4  .test/device-logs/hilog.397.20261029-110406.gz
E5  .test/device-logs/gpu-original-index.md
E6  .test/device-logs/meowvulkan.c
E7  .test/vulkan-host/baseline-results.txt
E8  .test/vulkan-host/final-tests.txt
E9  .test/vulkan-ohos/libmeowvulkan.so
~~~

### 2.1 游戏崩溃与时间线

已从设备实例只读拉取 10 份崩溃报告、29 个游戏日志文件、9 份原生故障记录及相关 GPU 归档。原件保存在被 Git 忽略的本地 .test/device-logs 目录，未纳入源码交付。

| 日期 | 崩溃时间（设备本地时间） | 共同首因 |
|---|---|---|
| 10-23 | 08:31:05 | DEVICE_LOST / wait semaphore |
| 10-27 | 20:43:27、22:47:37 | 同上 |
| 10-28 | 23:18:48 | 同上 |
| 10-29 | 00:09:23、11:04:37、12:18:06、13:38:33、13:46:10、21:40:35 | 同上 |

代表性原件：本地证据 E1（第4–16行）。调用链为：

~~~text
Minecraft.renderFrame
  → FrontendCommandEncoder.submit
  → VulkanCommandEncoder.submit
  → VulkanCommandEncoder.awaitSubmitCompletion
  → VulkanUtils.crashIfFailure
  → GpuDeviceLossException: VK_ERROR_DEVICE_LOST
~~~

**最新一次运行并没有再以同样错误终止：**本地证据 E2 在 21:41:49 初始化 Vulkan，之后持续运行到 22:09:05 的 Stopping!。因此不能说“设备当前正崩溃”或“每次启动都必崩”；本轮分析对象是保留下来的重复故障。

### 2.2 重新拉取的原始 hilog

除现有摘要外，本轮重新拉取两段压缩原件，解压后按可打印字符串定位关键记录：

- 本地证据 E3：解压 4,255,348 字节；偏移 346793、348705、350225、350843 处均出现 F72b push forwarded，原因均为 layout-not-captured。
- 本地证据 E4：解压 4,255,725 字节；偏移 908100 起出现下列 GPU 故障链。
- 本地证据 E5（筛选后的关键字符串索引） 只保留相关模板与偏移，便于复核，不转存其它系统日志内容。

~~~text
Msync do err flow begin
→ Gpu is reset when wait fences all
→ Recover error begin
→ Recover error end and return device lost
→ F69 vkWaitSemaphores -> vkWaitForFences
→ 3 条 HIGPUTDMD_HIGPU_PAGE_FAULT 事件记录
~~~

可确认：descriptor 回退出现在更早的连续归档片段，GPU 失败后同步等待看到错误。**可打印字符串保留的是模板与存储顺序，不是完整结构化时间戳/参数解码；不能据此给出精确毫秒差，也不能证明每条回退立即导致复位。** 三条 page-fault 事件记录也不直接等于三次独立 GPU 复位。

这两段原件未筛到 pipeline-layout table full / mirror table full。因此，虽然容量驱逐缺陷可以离线稳定复现，仍不能断言这次回退一定来自第 257 个布局；GIPA 漏捕获也是确定的候选来源。旧告警没有输出实际 pipeline layout handle，无法进一步一一关联。

### 2.3 源码对应关系

设备工作区留存的 本地证据 E6（Vulkan shim 源码原件） 与修改前本地源码 SHA256 完全一致：

~~~text
0e79a2c00cfd98f729a993f222e10a68d37f02f8f16354aced9e1b4a034bc640
~~~

这证明本轮审查针对相同的留存源码；没有通过部署实验重新验证设备实际加载二进制的全部构建来源。

## 3. 为什么布局跟踪会导致危险回退

主链见 [生产 shim](../../libs/meowcraftlib/src/main/cpp/meowcraftbridge/meowvulkan.c#L2915-L3010)：

~~~text
vkCmdPushDescriptorSet(KHR)
  → F72 普通 descriptor set 模拟
  → lookup(pipelineLayout, set) 查原 DSL
  → 找 mirror，分配 set，写 descriptors，绑定 set
  → 查不到布局 / 没有 mirror / 分配失败
  → 返回失败给上层 wrapper
  → 转发原生 vkCmdPushDescriptorSet
~~~

这个原生路径恰好是 shim 历史注释中希望规避的路径。**回退不是“无害地少打一条日志”，而是切换了实际记录 GPU 描述符的实现。** 如果描述符状态、layout 或资源寿命不成立，故障通常在 GPU 消费 command buffer 时暴露，随后 CPU 在 wait 处收到 DEVICE_LOST。

旧实现存在三类可复现缺陷：

### A. 将活跃对象元数据当作可驱逐缓存

pipeline layout 与 DSL 都只有 256 项。超额会驱逐最老项；DSL 驱逐还会销毁 shim 自建的 mirror。Vulkan 并没有保证应用最多同时拥有 256 个此类对象。

- 早期仍在用的 pipeline layout 丢映射 → layout-not-captured。
- 早期 DSL 的 mirror 被容量驱逐销毁 → no-mirror-layout / 已录制资源状态风险。
- 单纯把 256 改成更大的固定数只会推迟触发，不能修复对象寿命错误。

### B. pipeline layout 销毁不清理，句柄复用取到旧 DSL

旧实现捕获 CreatePipelineLayout，却没有 DestroyPipelineLayout 包装。死对象一直占槽；驱动再次分配同值 handle 时，capture 因“已有 key”直接返回，留下旧映射。

还要区分“销毁后复用”和“多个存活对象使用相同 non-dispatchable handle”：未启用 privateData 时，驱动可对相同对象定义进行 intern。后者需要按创建/销毁次数引用计数，而不能第一次 destroy 就删掉记录。

### C. GIPA / GDPA 两种合法查询路径不一致

Vulkan 设备命令既可通过 GetDeviceProcAddr 查询，也可通过带有效 instance 的 GetInstanceProcAddr 查询。旧 GIPA 只包装了少数设备命令；布局创建等调用可得到真实函数地址，完全绕过 capture，而 push 又得到 shim wrapper。

因此只要应用混用查询路径，即使只有一个布局，也能出现 layout-not-captured。这不依赖“玩家开了过高实体距离”等负载条件。

## 4. 本轮实际修改

修复分支为 **fix/vulkan-layout-tracking**；源码修复、回归测试、分析文档分别提交，只推送至用户 fork，不修改上游 main。

| 修改 | 实现位置 | 行为 |
|---|---|---|
| pipeline layout 动态记录 | [capture / forget](../../libs/meowcraftlib/src/main/cpp/meowcraftbridge/meowvulkan.c#L2140-L2220) | 按需增长，复用空槽，不驱逐存活项；缓存索引每次检查 capacity/key |
| DSL / mirror 动态记录 | [DSL 记录](../../libs/meowcraftlib/src/main/cpp/meowcraftbridge/meowvulkan.c#L2230-L2365) | 扩容不销毁其它 mirror；同值活跃句柄按 liveRefs 管理 |
| pipeline layout destroy | [创建与销毁包装](../../libs/meowcraftlib/src/main/cpp/meowcraftbridge/meowvulkan.c#L3962-L3984) | 最后一次有效 destroy 才移除记录，原 allocator 继续传给真实 Vulkan |
| metadata OOM | [创建包装](../../libs/meowcraftlib/src/main/cpp/meowcraftbridge/meowvulkan.c#L3962-L4015) | 不返回“已创建但没跟踪”的成功对象；回滚新 Vulkan 对象、清空 out、返回 OUT_OF_HOST_MEMORY；旧对象不变 |
| 纯 GIPA 初始化 | [CreateDevice 包装](../../libs/meowcraftlib/src/main/cpp/meowcraftbridge/meowvulkan.c#L6575-L6586) | 创建设备后记住真实 device/GDPA，不再依赖之前发生过 GDPA 查询 |
| 单一 wrapper 表 | [设备 wrapper 路由](../../libs/meowcraftlib/src/main/cpp/meowcraftbridge/meowvulkan.c#L6653) | GIPA/GDPA 共用 create/destroy/submit 等包装，保留已有 core/KHR 映射 |
| 可用性保护 | [查询导出](../../libs/meowcraftlib/src/main/cpp/meowcraftbridge/meowvulkan.c#L6820) | GIPA 不给不可用的非模拟扩展伪造 wrapper；明确的 sync2/push 模拟为例外；拒绝不存在的 Create/DestroySemaphoreKHR |
| 自检标识 | [初始化](../../libs/meowcraftlib/src/main/cpp/meowcraftbridge/meowvulkan.c#L6284) | 构建标识更新为 2026-11-01.84 live-layout-tracking |

shutdown 销毁池和自建 mirror，释放动态表并清零 capacity；重复调用及重新初始化已测试。**本轮没有改变真实 DEVICE_LOST 的传播，也没有开启 DROP_DRAW、DROP_PUSH_DESCRIPTOR 或 ring 复用来掩盖故障。**

## 5. 验证结果与复现方法

测试直接 include 生产 shim，不是复制一份另写的布局算法；替换真实 loader 与 hilog，完全不访问 GPU。见 [回归源码](tests/test_layout_tracking.c)、[运行脚本](tests/run_host_tests.sh) 与 [hilog stub](tests/stubs/hilog/log.h)。

最终 8 组全部通过，启用 ASan、UBSan 和 LeakSanitizer：

| 用例 | 验证目标 |
|---|---|
| lookup | 19 个关键设备命令的 GIPA/GDPA 一致、KHR 映射、未知名与 hooks-off 透传 |
| gipa-only | 没有 GDPA 查询时，GIPA 创建设备及 pipeline layout 仍能正确初始化和捕获 |
| gipa-unavailable | 不支持的 swapchain 命令保持 NULL；非法 semaphore KHR 名不伪造可用 |
| pipeline-capacity | 同时创建 1024 个布局，首个到最后一个映射全部保留，set 索引边界正确 |
| descriptor-capacity | 1024 个 push DSL 的 mirror 不被扩容销毁；交错销毁与重复 teardown 正确 |
| handle-reuse | destroy 删除映射，同值 handle 重新创建后使用新 DSL，跨设备周期不读旧记录 |
| interned-handles | 无空洞及前方有空洞时的相同活跃 handle 创建计数；DSL 共享 mirror 最后才释放 |
| oom | 首次/扩容分配失败回滚；256 个存活 pipeline/DSL 后第257次扩容失败不破坏旧映射与 mirror；原 allocator 保留 |

对设备拉取的旧源码运行同一套测试中的 lookup、pipeline-capacity、descriptor-capacity、handle-reuse、oom，**5 组均触发预期断言失败**；修复版对应通过。这证明本轮改动修复了可观察行为，不只通过语法检查。对照结果见 本地证据 E7，最终结果见 本地证据 E8。

另外已完成：

- OHOS clang 的 aarch64-linux-ohos 语法检查。
- 真实目标平台 shared-library 编译/链接，启用 --no-undefined。
- ELF 检查：ELF64、AArch64、DYN，仅依赖 libhilog_ndk.z.so 与 libc.so，没有硬链接 libvulkan。
- Git diff --check。
- 独立只读复审；补齐其提出的 GIPA 可用性、intern 句柄计数和 OOM 测试边界。

本地构建验证件：本地证据 E9。它没有签名、没有替换随包库、没有打入 HAP，也没有推送到设备；仅用于证明本轮源码可在目标平台编译链接。

在本机 WSL 复跑：

~~~bash
VULKAN_HEADERS=/mnt/d/apps/ohos_sdk/26.0.0/native/sysroot/usr/include \
    bash tools/vulkan/tests/run_host_tests.sh
~~~

Linux 也可使用其它官方 Vulkan headers 目录及 CC。脚本不自动下载 SDK、安装依赖或执行 hdc。宿主 mock 不验证 Maleoon 驱动执行结果，也不代表已经通过 Vulkan validation layer。

## 6. 重要的未修缺口及下一步优先级

以下是源码审查得到的合法输入反例，**不是本轮已通过 GPU 实验的根因结论**。为保持这次修复可审查、可回归，没有同时重写整个 Vulkan 翻译器。

### P0：descriptor layout 翻译不闭合

[创建 mirror 时去掉 push flag](../../libs/meowcraftlib/src/main/cpp/meowcraftbridge/meowvulkan.c#L2325-L2335)，而 [普通 set 仍绑定到原 pipeline layout](../../libs/meowcraftlib/src/main/cpp/meowcraftbridge/meowvulkan.c#L2998-L3009)。创建 flags 已不同，不能再宣称两个 DSL identically defined。

已核实 Khronos [VUID-00358](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdBindDescriptorSets.html) 及 [identically-defined 定义](https://github.com/KhronosGroup/Vulkan-Docs/blob/main/appendices/glossary.adoc)。原源码中的“push bit 不参与兼容性”注释已更正，**行为本轮未修**。

正确方案不是只改 bind 的一个 handle：需要统一下层普通 pipeline layout、graphics/compute pipeline、push constants、普通绑定及模板路径。翻译为普通布局之后不能再把 native push 发给它，因此必须一起处理失败回退。建议先用 validation layer / fake ICD 的 flags 检查复现，再做独立闭环补丁。

另一个相关寿命缺口：应用在创建 pipeline layout 后可销毁原 DSL，当前 [forget_dsl](../../libs/meowcraftlib/src/main/cpp/meowcraftbridge/meowvulkan.c#L2353-L2364) 仍随原 DSL 最终销毁 mirror。pipeline 应持有所需 DSL 元数据/mirror 的独立引用。本轮 liveRefs 仅解决相同原始 handle 的多次 create/destroy，不是 pipeline→DSL 引用计数。

### P0：fence 共享所有权与错误完成证明

- [F69 add_entry](../../libs/meowcraftlib/src/main/cpp/meowcraftbridge/meowvulkan.c#L1862-L1888) 满64项直接驱逐旧 fence；未普遍证明 fence 已完成。
- 同一 submit 的 fence 可记录在多个 timeline map；[forget_sem](../../libs/meowcraftlib/src/main/cpp/meowcraftbridge/meowvulkan.c#L1978-L1987) 却逐 entry 销毁。现有 F72 borrow 计数并不等于完整 map-owner 计数，存在悬空/重复销毁反例。
- [WaitSemaphores](../../libs/meowcraftlib/src/main/cpp/meowcraftbridge/meowvulkan.c#L3746-L3885) 对未充分映射的目标可用 last_queue idle 代替目标 semaphore/value 的完成证明。未来值、其它 queue、host signal 都不能由这次 idle 推导。
- WAIT_ANY、initialValue、host signal、counter 单调性及 value=0 timeline 提交仍需单独补齐。

建议统一真实 fence 的 owners / pending / completed 状态；成功只由目标完成条件证明，保留 timeout 与负错误传播。先增加“两条 timeline 共用 fence”“65个在途 entry”“future value + 空闲 queue”等 mock，不能用吞错来延长运行。

### P1：partial push、command buffer 与 queue 归属

[模拟 push](../../libs/meowcraftlib/src/main/cpp/meowcraftbridge/meowvulkan.c#L2915-L3010) 每次 fresh set 只更新本次 writes。合法的“先写 binding0，再写 binding1”需要继承前次 descriptor 状态，当前 full-coverage gate 只限制 ring 复用，不解决默认路径。

[池绑定到 submit](../../libs/meowcraftlib/src/main/cpp/meowcraftbridge/meowvulkan.c#L2871-L2911) 依赖全局 used，而不是实际提交的 command buffers。多 CB 预录制、多个 queue 交错时，当前完成证明可能覆盖不足。它们可能造成 GPU 继续使用已经 reset 的 set，具备触发 page fault 的机制，但没有这次 MC 的 CB/queue trace 来证明发生。

修复应记录 CB→pool generation 和每次实际 submit uses；只在全部使用完成且不存在待提交引用时回收。单设备/单渲染线程约定也不能当作通用 Vulkan 并发保证；本轮新增表仍沿用该约定，没有宣称实现多设备/多线程支持。

### P1：submit 翻译的边界输入

默认合并会把 waits 前移、signals 后移，需证明 entries 没有内部 semaphore 依赖；否则 signal→wait 的合法序列可能被合并成错误等待。诊断 split 路径又有重复使用同一非零 fence、部分成功后整体失败清理的反例。建议用独立提交图 mock 覆盖后修复，而非随意切换 MERGE 开关当作根治。

## 7. 其它报错为什么不是同一个故障

| 报错 | 解释与处置 |
|---|---|
| JNA native 加载 Invalid argument、OSHI NoClassDefFoundError | 本地证据 E2（第142–210行） 显示 JNA native 初始化失败，随后依赖类初始化失败；系统信息查询被上层捕获。启动器 [OSHI 覆盖逻辑](../../entry/src/main/ets/common/data/MinecraftLauncher.ets#L1458-L1476) 本就区分已适配件与保留 upstream 时的 JNA noise。需要对失败 native 的 ELF/loader 原因继续核查，不能由 Invalid argument 一句直接确定签名或 ABI 细因。不是 DEVICE_LOST 栈的直接首因。 |
| Narrator / flite 初始化失败 | 本地证据 E2（第385–421行） 继续依赖已失败的 JNA；影响朗读功能，之后游戏仍继续运行。 |
| Failed to acquire image / swapchain out of date | 本地证据 E2（第476–486行） 发生在窗口配置变动附近；后续继续连接并运行到正常停止，不是同义于 GPU lost。 |
| 可选模组 ClassNotFound、mixin 禁用 | 需要分别看目标模组是否必需；不能把兼容检查/可选依赖警告直接判为进程崩溃首因。 |
| 原生 faultlog 中 SIGSEGV @0x8 / HotSpot code cache | HotSpot 使用隐式空指针陷阱；shim [已有说明](../../libs/meowcraftlib/src/main/cpp/meowcraftbridge/meowvulkan.c#L6286-L6300)。单看这一类系统记录不能等价于最终 JVM 致命崩溃，应结合 JVM fatal log 与同次运行是否继续；本轮没有重新把它们认定为 GPU 根因。 |

额外发现两个独立集成问题，已记录但本轮未改：

1. [LWJGL 通用重编入口](../lwjgl/rebuild_for_meowcraft.sh#L57-L69) 不会自动纳入 [单件 F2 aligned_alloc 修复](../lwjgl/build_lwjgl_core_aligned_alloc_fix.sh#L116-L165)，未来重编可能覆盖已修 core。当前随包 native 清单校验一致，没有证据显示现在已被覆盖。它对应 CPU allocator NULL，不能直接解释本次 GPU page fault。
2. [SDL Vulkan 的窗口准备假设](../sdl/src/video/ohos/SDL_ohosvulkan.c#L282-L289) 与 [bridge 已将 usage 准备移到 GLFW 独有路径](../../libs/meowcraftlib/src/main/cpp/meowcraftbridge/egl_gl.c#L1466-L1476) 不一致。26.3 SDL 路径会漏掉该策略；需要统一 Vulkan surface 准备入口并验证，未证明其导致本次复位。

## 8. 后续验证顺序

1. 先用宿主 Vulkan validation / fake ICD 补 descriptor compatibility、partial push 与目标 fence 完成证明测试，逐项修复第6节的确定语义缺口。
2. 有用户后续部署授权时，再验证设备实际加载标识、记录布局 handle、模拟/回退计数、CB/queue/fence 关联，以及首次 GPU 错误前最后一组提交。本轮没有部署，因此此步骤尚未执行。
3. 负载对照单独做：设备当前 entityDistanceScaling=5.0、maxFps=120、VSync=true、renderDistance=10、simulationDistance=6。可用实体距离 1.0、限60帧、降低渲染分辨率作为控制变量，**本轮没有修改这些配置**。降载后不崩只是缩小触发条件，不证明实现正确。
4. [渲染策略](../../entry/src/main/ets/common/data/RendererPolicy.ets#L69-L91) 只有显式 Vulkan 才请求原生 Vulkan；默认 auto 不是这条路径。[GL/GLES 后端也可能经 Mesa/Zink 到 Vulkan](../../entry/src/main/ets/common/data/GameLauncher.ets#L255-L258)，因此“切 GL”可隔离本项目 shim，但不能证明绕开底层 Maleoon Vulkan 驱动。

## 9. 用户设备实测清单

1. 从用户 fork 获取 fix/vulkan-layout-tracking 分支后，重新构建 meowcraftlib / meowjre 对应原生库与 HSP；不要只更新 ArkTS 代码或继续使用旧缓存构建件。
2. 在 hilog 中确认实际加载标识为 **2026-11-01.84 live-layout-tracking**，再判断新版本行为。
3. 第一组保持原游戏、模组、场景、分辨率和启动环境，先不降实体距离或限帧，便于与旧版对照。保留错误传播，保持 ring 复用、DROP_DRAW 和 DROP_PUSH_DESCRIPTOR 关闭。
4. 需要低频统计时可单独设置 MEOW_VK_TOTALS_SEC=10，观察 emulated / forwarded 与池计数；逐调用 MEOW_VK_VERBOSE=1 会显著改变日志量和时序，不建议作为首次对照的默认配置。
5. 若仍崩溃，保留本次启动标识、崩溃报告和首次 GPU reset 前后 hilog，重点比对 layout-not-captured / no-mirror-layout / allocate-set-failed 是否仍出现，以及 DEVICE_LOST 的先后关系；不要仅凭运行更久就宣布根治。

**交付边界：已完成证据分析、局部源码修复、8组离线回归及 OHOS 编译链接；源码按块发布至用户 fork，原始日志与本地验证二进制不入库。没有修改设备状态，也没有把局部通过包装成 GPU 故障根治。**

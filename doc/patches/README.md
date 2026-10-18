# 引擎工作区整理说明（2026-10-11）

本目录记录一次对工作区未提交内容的整理，以及整理前后的**完整快照**，任何改动都可原样还原。

## 一、结论：工作区里三类内容

| 类别 | 判定依据 | 处理 |
|---|---|---|
| **项目自己的在研工作** | 改动共 5669 行，而排查期加入的标记只有约 500 处 ⇒ 绝大部分改动**不是本次排查产生的** | **一律不动** ✓ |
| **真修复**（保留生效） | 见下表 | 保留 ✓ |
| **不确定的修复 + 诊断码** | 全部带 `URGE_*` / `TEMPORARY` 标记 | 代码**原样留在树里** ✓、默认值全部改为**关** ✓、并已存快照 ✓ |

### 保留下来的"真修复"

| # | 修复 | 位置 | 证据 |
|---|---|---|---|
| 1 | **`Ox` / `Oy` 真的变化才置脏**（此前无条件置脏 ⇒ 每帧全量重解析地图 + 重传整批顶点） | `content/render/tilemap_impl.cc`（标记 `URGE_OXDIRTY`） | 上传从 **5630 次/4 分钟 ⇒ 0 次/10 秒** ✓ |
| 2 | **精灵参数下标取自顶点色 `.x`**（替代 `SV_VertexID`） | `renderer/pipeline/builtin_hlsl.cc:489` | 这颗 Mali 驱动每次绘制返回同一个 `SV_VertexID` ⇒ 所有精灵读同一参数 ⇒ 位置/尺寸/颜色全乱 ✓ |
| 3 | **ClipBlt 角标取自顶点色 `.z`**（替代 `SV_VertexIdx`） | `renderer/pipeline/builtin_hlsl.cc:220` | 同上；取同一角 ⇒ 该角 alpha 为 0 时整元素丢一帧 ✓ |

> 修复 2、3 是项目自身未提交的修正，曾被一次整文件 `git checkout` 误删，已逐字恢复 ✓。
> **教训与规矩：撤代码只做定点删除，绝不整文件 `checkout`** ✓。

### 默认已关的实验开关（仍可用开关文件单独开启）

| 开关文件（`…/files/urge_debug/`） | 内容 | 实测结论 |
|---|---|---|
| `tmstore.txt` | 帧尾额外两次 pass 收开，不重画 | 画面中性 ✓，但**未消除**缺陷 ✗ |
| `dsreclear.txt` | 绘制前额外清一次深度 | 画面中性 ✓，但**未消除** ✗ |
| `tmsecondpass.txt` | 地图逐层"就地重画一遍" | **会二次混合半透明内容** ⇒ 画面可变 ✗、且未消除 ✗ |
| `sprredraw.txt` | 精灵批"就地重画一遍" | 同上 ✗ |
| `sprnodepth.txt` | 精灵管线改用"不测深度"版本 | 可能改变精灵互相遮挡 ✗、未消除 ✗ |
| `vpcopydraw.txt` | 视口合成由拷贝改为绘制 | 画面逐点一致 ✓，但该 pass 在明亮地图不执行 ⇒ 无关 ✗ |
| `skip_window.txt` / `skip_window2.txt` / `skip_plane.txt` / `skip_sprite.txt` | 按类跳过绘制（诊断用） | 会改变画面，仅供诊断 ✓ |
| `key.txt` | 按键钩子：写 SDL scancode，可写一串（突发模式，~30 次/秒） | 自动化测试用 ✓ |

开关文件**读一次即删除** ✓；不带文件时使用代码里的默认值 ✓。

### 诊断码（暂存，未删除）

`[PassAudit]`（每帧 pass 数 / 边界来源 / 目标切换）、`[CellProbe]`+`[CellAll]`+`[CellUI]`（画面异常自动判定与转储）、`[WndAudit]`、`[SprAudit]`、`[UploadAudit]`、`[LayerAudit]`、`[RunFp]` 等约 500 处标记；
另有 `third_party/DiligentCore` 内的 `URGE_PASS_AUDIT` 等计数器（`VulkanUtilities/CommandBuffer.hpp`、`DeviceContextVkImpl.{hpp,cpp}`）✓
⇒ 这些是后续继续排查的"眼睛"，**保留在树里** ✓；如需彻底摘除，见 `2026-10-11_worktree.patch` 与 `2026-10-11_diligentcore.patch` 可完整还原 ✓。

## 二、快照与还原

| 文件 | 内容 | 校验 |
|---|---|---|
| `2026-10-11_worktree.patch` | 整理**之前**的完整工作区改动（7368 行） | `git apply --reverse --check` 通过 ✓ |
| `2026-10-11_diligentcore.patch` | 子模块 `DiligentCore` 的改动（402 行） | 同上 ✓ |
| `2026-10-11_status.txt` | 整理前的 `git status --short` | — |
| `2026-10-11_stash_list.txt` | 当时的 stash 列表（含 10-02 的一份 WIP ✓） | — |

**还原办法**（任何一步都可回退）：

```bash
# 查看某次改动
git apply --stat doc/patches/2026-10-11_worktree.patch

# 反向还原（把树恢复成快照时的样子）
git apply --reverse doc/patches/2026-10-11_worktree.patch
git -C third_party/DiligentCore apply --reverse ../../doc/patches/2026-10-11_diligentcore.patch
```

## 三、下一步（缺陷本身）

缺陷已定位到**上屏合成**这一环（帧先渲染到离屏 `screen_buffer`，再由 ImGui 当纹理采样画到交换链 —— "写后立即读" ✓，见主文档 §15 ✓）。

1. **选项 A**：合成前加**显式屏障 + 状态往返**（RT→SRV→RT）
2. **选项 B**：结构性改造 —— 帧直接渲染进交换链后缓冲，使该环不存在

---

# 2026-10-15 追加：定位结论改变、修复落地、诊断码已摘

主文档的 **§18 / §19** 是这两天的完整记录。与本目录相关的要点：

## 四、上文两处结论已被推翻，注意不要照旧引用

| 上文说法 | 现状 |
|---|---|
| "缺陷已定位到**上屏合成**这一环" | **已推翻** ✗。加了"合成前读 `screen_buffer`"的第二份回读后，12/12 次事件在**合成前就存在** ⇒ 缺陷在"**渲染进 `screen_buffer`**"这一步（主文档 §18.2） |
| `tmsecondpass.txt` / `sprredraw.txt` "**未消除** ✗" | **已推翻** ✗。用新判据（`kUrgeCellBig=20`，噪声与缺陷之间完全空档）复测：两者都**真有效**，三个重画合起来把菜单症状打到 **0 ~ 1 次 / 75 秒**（基线 9 ~ 16），主文档 §18.5 / §19.5 |

## 五、已落地的修复（**按 GPU 家族门控**）

| 文件 | 开关文件 | 门控默认 |
|---|---|---|
| `content/render/tilemap_impl.cc` | `tmsecondpass.txt` | `renderer::IsMaliGpu()` → **开** |
| `content/render/window_impl.cc` | `wndredraw.txt` | 同上（**背景层 + 内容层都重画**） |
| `content/render/sprite_batch.cc` | `sprredraw.txt` | 同上 |

并且**修复了这三种重画共用的一道边界**（这是修复本体的一部分，不是诊断码）：

- `content/screen/renderscreen_impl.cc` 的 `UrgeForcePassBoundary()` 原先**永远重开"帧的屏幕 target"**。而 pass 不只为帧而开 —— `ViewportImpl::Render(bitmap)` 会在它拿到的 Bitmap 上开 pass（标题镜子的 `@sprite_mirrorsrc_vp.render(@target)` 就是这样）。在那一趟里触发重画 ⇒ 第二笔被送到**屏幕**上、并被视口矩形裁住 ⇒ **标题界面左侧闪出未遮罩的镜子动画一帧**。
- 修法：新增 `UrgeSetPassTarget(rtv, dsv)`，由**每个开 pass 的地方**上报自己刚绑定的 target（帧 pass、视口/`Viewport#render` 的 Bitmap），边界改为重开**上报的那个**；窗口与地图各自的边界 helper 也统一改走它。见主文档 §20.5。

- 有效性：标题「时代编年」进场闪帧 **修复前 1~2 次/次进场 → 修复后 0**（`spr` 开或关都为 0）；菜单 120 秒手速扫动密采样大事件 **0**（基线 15 ~ 30）✓
- 代价：静止 25 ms / 40 fps（与全关一致，顶在引擎上限）；扫动中 4 ~ 7% ✓
- 画面：菜单里逐格量到 53/256 格偏移，但**作者肉眼看不出** ✓
- **门控必须保留**：在手机上强制打开（用开关文件覆盖）是**纯负收益** —— 帧率无差别，却凭空多出约 25 ~ 30 次/分钟的一帧差异（基线 0）✓ 见主文档 §19.9
- 用户实机判定：**"修好了"** ✓（主文档 §20.7）

## 六、诊断码：**已全部恢复**（原为"已摘净"，因清理事故回退）

- 2026-10-15 下午尝试剔除诊断码时发生**六处真实代码误删**（主文档 §22）。当前工作树已恢复到诊断码齐全的状态。
- `third_party/DiligentCore` 重新应用了 `2026-10-14_diligentcore_investigation.patch`（3 个文件，`[PassAudit]` 等计数器可用）。
- 主仓的 `[CellProbe]` / `[CellBig]` / `[CellAll]` / `[CellDense*]` / `[WndAudit]` / `[BatchAudit]` / `skip_*` / `key.txt` 钩子 / `diag.txt` 门控 **全部在位** ✓
- `[FrameTime]` 在**清理版**里被一并删掉了，**当前基线也没有** —— 若需要可信帧率尺子，需从 `2026-10-15_KNOWN_GOOD_baseline.patch` 之外另行恢复（约 30 行，每秒一行，故意不受门控）。

## 七、当前状态与快照（2026-10-15 下午更新）

### ⚠️ 首要事项：诊断清理发生过事故，详见主文档 §22

剔除诊断码时，**六处真实代码被误删**（canvas 的 blit/clip 调用、plane 的 `IndexType`、tilemap 的 `UpdateBuffer` 与 above 层绘制、sprite 的 `MarkEmitted`）。其中 `MarkEmitted` 丢失会让精灵被**重复绘制**，正是"地图上的东西错位、跟着动"的直接原因。**当前工作树已全部恢复。**

**凌晨 4~5 点那一版的确切代码没有独立备份** —— `2026-10-15_worktree.patch` 在清理过程中被反复覆盖，原 9666 行版本已永久丢失。当前基线是**重建**的（Oct 14 快照 + 重新应用的修复）。

### 快照清单

| 文件 | 内容 | 校验 |
|---|---|---|
| **`2026-10-15_VERIFIED_baseline.patch`** | **✅ 当前基线**（7557 行）：**用户实机确认游戏内场景与镜子动画都正常** | 正/反向 `--check` 均通过 ✓ |
| `2026-10-15_diligentcore.patch` | 当前子模块（402 行） | 双向 ✓ |
| `2026-10-15_worktree.patch` | 与基线同内容（7557 行） | 反向 ✓ |
| `2026-10-14_investigation_switches.patch` | 10-14 完整状态（7512 行，诊断码齐全）—— 基线的基底 | ⚠️ 它是 `HEAD → 10-14` 的差分，**不能对当前工作树做反向 `--check`** |
| `2026-10-14_diligentcore_investigation.patch` | 子模块（402 行，与当前同内容） | 双向 ✓ |
| `superseded_2026-10-15_KNOWN_GOOD_baseline.patch` | ❌ **已作废**：重建过程中的中间版本（从未被确认正确） | 不要使用 |
| `superseded_2026-10-15_reconstructed.patch` | ❌ **已作废**：同上 | 不要使用 |
| `2026-10-11_*.patch`（6 个） | 更早的快照（10-11 前后，缺陷尚未修完） | 均通过 ✓ |

> ⚠️ **重要**：10-15 凌晨那一版（用户确认"修好了"）的快照在排查过程中被反复覆盖，**已永久丢失**。当前基线是重建的，但**经用户实机确认两个症状都消失**，可作为后续工作的起点。

### 当前基线（**用户实机确认：游戏内场景正常 + 镜子动画正常**）

| 项 | 值 |
|---|---|
| 基线快照 | **`2026-10-15_VERIFIED_baseline.patch`**（7557 行，正/反向 `--check` 均通过 ✓） |
| 主仓改动 | **35 个文件**，+5799 / −153 |
| 子模块 | `2026-10-15_diligentcore.patch`（402 行，**实测惰性**） |
| 诊断码 | **全部在位**（剔除诊断的工作尚未开始，见主文档 §23.7） |
| pass target 上报 | **3 处**：`renderscreen` 帧 pass + `viewport_impl.cc:921`（**仅此一处**，与历史记录的 +5/+4 逐行一致） |
| 三重画 | `tilemap` / `window` / `sprite_batch` 默认 `IsMaliGpu()` |
| 帧集轮转 / 静态参数缓冲 | 保留（两项都已实机验证为必需） |

### 还原基线的命令

```bash
git apply doc/patches/2026-10-15_VERIFIED_baseline.patch
git -C third_party/DiligentCore apply ../../doc/patches/2026-10-15_diligentcore.patch
```

> 若 apply 报 `already exists in working directory`，先 `rm` 掉被点名的文件（通常是两个新增头文件 `renderer/device/{device_mode,gpu_driver}.h`）再重试。

### 装机纪律（主文档 §23.4.2 / §23.6）

1. **只用 `adb install -r`** —— 曾用 `uninstall` 重装导致激活记录被删，只能从另一台设备复制 `Sysdata/_SystemSave.rxdata` 恢复。
2. 装机后必须三道校验才算装上：① 输出含 `Success`；② 电视上 APK 字节数 == 本地；③ 进程已重启。
3. **画面异常先查环境**：`/sdcard/Android/data/com.admenri.urge/files/` 下 `Save/ Data/ Scripts/ Extra/ Fonts/ Graphics/ Audio/ Sysdata/ geoid_map/` 与 `engine_i18n.xml` 是否齐全（资源不进 APK，正常包 35 MB）。

### 下一步删诊断：必须遵守（主文档 §22.6 + §23.6）

1. **一次只删一个文件**，删完立刻构建 + 装机 + 你验证
2. **绝不按分号切语句** —— 多行 `LOG` 之后那一行必须人工确认
3. **拿不准的一律不删**（任何 `render_context->` / `DrawIndexed` / `MarkEmitted` / 三元 / 赋值 / 控制流）
4. **不做推导性改动** —— 已验证的修复要改，先拿可核对的规格（增删行数/原文）照抄
5. **过滤式审计不可信** —— 审计要输出全部差异行供人眼过
6. **数值不是判据，画面才是**
7. 每个文件的删除**单独存 patch**，便于单文件回退

---

## 八、当前工作树与基线的关系（2026-10-16 更新）

### ⚠️ 当前代码 = 基线 + 16 处临时探针

为排查"回合面板接缝闪烁"（主文档 **§24**），在 `content/` 下加了 **16 处标记为 `URGE_*` 的临时探针**：

| 标记 | 处数 | 默认是否输出 | 涉及文件 |
|---|---|---|---|
| `URGE_QUADHASH` | 1 | **是**（每帧 2 行） | `window_impl.cc` |
| `URGE_CANVASOPS` | 4 | **是**（每帧 2 行） | `canvas_impl.{h,cc}`、`window_impl.cc` |
| `URGE_DRAWORDER` | 1 | **是**（每帧 2 行） | `window_impl.cc` |
| `URGE_TEXHASH` | 3 | 否（`diag.txt` 门控） | `canvas_impl.{h,cc}`、`window_impl.cc` |
| `URGE_FRAMESET` | 5 | 否（仅索引不一致时） | `window_impl.{h,cc}` |
| `URGE_DUPCOUNT` | 2 | 否（仅某层重复绘制时） | `window_impl.{h,cc}` |

- **它们不改变渲染行为**（只是读数据 + 打日志），但前三个**默认就会输出**；
- 探针涉及 **4 个文件**：`content/render/window_impl.{cc,h}`、`content/canvas/canvas_impl.{cc,h}`。

### 回到干净基线的两种方式

```bash
# 方式一：只回退探针涉及的 4 个文件（推荐 —— 其余保持当前状态）
git checkout HEAD -- content/render/window_impl.cc content/render/window_impl.h \
                     content/canvas/canvas_impl.cc content/canvas/canvas_impl.h
git apply --include='content/render/window_impl.cc' --include='content/render/window_impl.h' \
          --include='content/canvas/canvas_impl.cc' --include='content/canvas/canvas_impl.h' \
          doc/patches/2026-10-15_VERIFIED_baseline.patch

# 方式二：整树回退到基线
git checkout -- .
git apply doc/patches/2026-10-15_VERIFIED_baseline.patch
git -C third_party/DiligentCore apply ../../doc/patches/2026-10-15_diligentcore.patch
```

### 本轮排查的结论索引

| 内容 | 位置 |
|---|---|
| 现象量化（位置 / 两个取值 / 触发条件 / 机制） | 主文档 §24.1 |
| **12 个已证伪的假设**（附判据，别重复试） | 主文档 §24.2 |
| **全部"恒定项"清单**（10 项） | 主文档 §24.3 |
| 临时探针清单（16 处） | 主文档 §24.4 / 本节 |
| 方法与教训（抓单帧、网格粒度、判据方向） | 主文档 §24.5 |
| 下一步唯一方向（扫全所有窗口/层的绘制次数） | 主文档 §24.6 |

### 设备侧状态

| 项 | 值 |
|---|---|
| 电视包 | `35,924,300` 字节（本轮最后一次构建，含全部探针） |
| 游戏 | 已关闭（`am force-stop`） |
| `urge_debug/` | 已清空（日志与 `cellprobe_*.dat` 全部删除） |

---

## 九、2026-10-16 夜更新：当前树 = 基线 + **1 处真实修复** + 一批探针

### ✅ 真实修复（行为变更，不是观感调整）

| 位置 | 内容 |
|---|---|
| `content/render/drawable_controller.cc` | 遍历时逐级检查**父视口可见性**（`ViewportInfo::visible`） |
| `content/render/drawable_controller.h` | `ViewportInfo` 增加 `visible` 字段 |
| `content/screen/viewport_impl.cc` / `renderscreen_impl.cc` | 每帧把视口自身节点的可见性同步进 `ViewportInfo` |

**为什么**：视口的 `visible` 只写在自己那个节点上，从不传给子节点，遍历也只看节点自己的 `visible_`
⇒ **隐藏视口后里面的窗口照样被画**。实测后果：战斗中地图整套 UI 继续渲染，其 gold 面板与战斗回合
面板同坐标叠加；修好后右侧文字闪烁消失（用户确认），同位置重复窗口由 2 个降为 1 个。

详见主文档 **§25.1**。

### 探针（只读，不改渲染）

| 标记 | 处数 | 说明 |
|---|---|---|
| `URGE_SNAPSHOT` / `URGE_DISPATCH` | 2 | 每帧列出遍历快照、分发决策（主文档 §25.3 的定位靠它） |
| `URGE_WINKIND` / `URGE_TEXTWIN` / `URGE_NODE` | 3 | 窗口↔节点映射、采样区窗口、节点可见性/z |
| `URGE_QUADHASH` / `URGE_TEXHASH` / `URGE_CANVASOPS` / `URGE_DRAWORDER` / `URGE_FRAME**` / `URGE_DUPCOUNT` | 若干 | 今晚排查接缝时的输入恒定性核对 |
| `PixelProbe`（`renderscreen_impl.cc`） | 1 | 每帧回读 128×32 区域；**锚点已改为右侧文字区**（内部 661,219） |

### ❌ 已回退、不要重做的实验（见 §25.2）

- `rtkeep`（画布每条命令后不���绑渲染目标）：接缝稳定，但**菜单文字变淡**、右侧文字闪 ⇒ 默认已关
- 帧首"清除前推 scissor"：为错误假设所加，未验证 ⇒ 已回退
- 窗口重画"没几何就不切断 pass"：接缝稳定，但**右侧文字闪**（用户确认）⇒ 已回退

### 回到干净基线

```bash
git checkout -- .
git apply doc/patches/2026-10-15_VERIFIED_baseline.patch
git -C third_party/DiligentCore apply ../../doc/patches/2026-10-15_diligentcore.patch
```

⚠️ 注意：这样会把上面的**视口可见性修复**一并回退 —— 若要保留它，请重新按 §25.1 的三处改动补回。

---

## 十、2026-10-16 深夜更新：接缝修复（最终形态）

### ✅ 本次的行为改动（共 2 处，均已验证）

| 位置 | 内容 |
|---|---|
| `content/canvas/canvas_scheduler.{h,cc}` + `content/canvas/canvas_impl.cc` | **把画布的"解绑渲染目标"从每条命令一次收敛为每批次一次**（`BeginCommandBatch`/`EndCommandBatch`；批次内只记账，批次末统一解绑一次；批次外行为不变） |
| `content/render/drawable_controller.{h,cc}` + `content/screen/viewport_impl.cc` / `renderscreen_impl.cc` | 遍历时逐级检查**父视口可见性**（`ViewportInfo::visible`）——隐藏视口不再继续绘制里面的窗口 |

**根因（一句话）**：画布在**每条绘制命令后**解绑一次渲染目标 ⇒ 每帧十几次 render scope 结束；
瓦片式 GPU（Mali）每次边界都要 store/reload 屏幕 tile ⇒ 两个半透明窗口层重叠那条带的**混合次数逐帧不同**
⇒ 输入逐字节相同却输出两个值。GLES 无 render scope ⇒ 别的平台不出现。

**为什么不是"直接跳过解绑"**：跳过会把画布位图留在渲染目标上，导致 **L 系统菜单只剩空框**、右侧文字闪烁。
必须保留"画完交还目标"的语义，只能**降频**。

### 验证结果

| 项 | 结果 |
|---|---|
| 接缝 | 300 帧**单一值**，切换 **0** |
| L 系统菜单 | 内容完整 |
| 右侧文字区 | 12 帧差异 **0 px** |

⚠️ 带的绝对值 89 → **126**（仍单一值）。观感请用户确认。

详见主文档 **§26**。

### 仍然存在的探针

`URGE_*` 标记的临时探针仍散布在 `window_impl.cc` / `canvas_impl.cc` / `canvas_scheduler.cc` /
`renderscreen_impl.cc` / `drawable_controller.cc` / `viewport_impl.cc`（只读、不改渲染，但部分默认每帧输出）。
清理时按标记搜索即可：`grep -rn "Marker: URGE_" content/`。

---

## 十一、收工状态（2026-10-16）

### 电视上的包

**35,935,567** = 基线 + **一处行为修复**（隐藏视口不再继续绘制里面的窗口）。
L 菜单正常、右侧文字不闪；**接缝仍闪**（未声称修好）。

### 判据工具

```bash
python tools/seam_metric.py <截图...>     # 参照值 ≈0.10（来自 PJZ110/Adreno）
```

详见主文档 §27（为什么以前那套判据无效）与 §28（交接与下一步）。

### ⚠️ 关于回退

README 第八节描述的"当前代码 = 基线 + 16 处探针"已过时。
现在树里的探针比那时更多（今晚排查接缝期间加的），**行为改动只剩一处**（视口可见性）。
清探针的方式：

```bash
grep -rn "Marker: URGE_" content/          # 列出全部探针位置
git checkout -- .
git apply doc/patches/2026-10-15_VERIFIED_baseline.patch
git -C third_party/DiligentCore apply ../../doc/patches/2026-10-15_diligentcore.patch
# 之后按 §25.1 / §26 重新补回视口可见性修复（若还需要）
```

---

## 十二、接缝修复（最终）：`sprredraw` 默认改回关闭

**改动**：`content/render/sprite_batch.cc` — `UrgeSpriteRedrawEnabled()` 的默认值
`renderer::IsMaliGpu()` → **`false`**（与它自己的注释结论一致；`sprredraw.txt=1` 仍可开启对比）。

**依据**：电视 12 帧对照实验 —— `sprredraw=1` 时 8/12 帧接缝相对深度 0.210（应为 0.098）；
`sprredraw=0` 时 12/12 帧 0.098，与手机参照（0.105）一致。

**验证（包 35,935,643，默认配置）**：接缝 12/12 ✓、L 菜单内容完整 ✓、右侧文字 10 帧 0px ✓。

详见主文档 §29。

---

## 十三、右下角图标单帧丢失：修复（清除必须在 scissor 之后 → 之前）

**改动**：`content/screen/renderscreen_impl.cc` —— 帧首**先推全屏 scissor，再清除**。

**根因**：清除被 scissor 裁剪（Vulkan: `vkCmdClearAttachments`），
只清掉上次 pass 残留的矩形；其余区域保留上一帧的**深度**；
精灵管线保留深度测试 ⇒ 精灵被拒绝 ⇒ 那片区域整帧不画（罗盘消失一帧）。

**验证（包 35,934,095）**

| 项 | 结果 |
|---|---|
| 罗盘单帧丢失（`tools/frame_loss.py`） | **0 / 297**（修复前 17 / 296）|
| 接缝相对深度（`tools/seam_metric.py`） | 12/12 = 0.098（参照 0.105）|
| L 菜单 / 右侧文字 | 完整 / 0 px |

详见主文档 §31。

---

## 十四、"只重画位图型精灵"已试并回退（§32）

结论：位图精灵同样有半透明区域（战斗背景即其中之一），只重画它们会把接缝打回 0.210。
已完全回退；`sprredraw` 默认仍为关闭。

现状（包 35,936,330）：接缝 0.098（参照 0.105）✓、L 菜单 ✓、右侧文字 ✓、罗盘偶发丢帧（0.2~1%）。

---

## 十五、只重画罗盘纹理（同时满足接缝与罗盘）

**改动**：`content/render/sprite_impl.cc` 两处绘制点 —— 当 `texture->name` 含 `compass` 时，
额外重画一次（等同 `sprredraw` 对该纹理生效），其余精灵不变。

**实测（包 35,937,307）**：接缝 12/12 = 0.098（参照 0.105）✓、L 菜单完整 ✓、罗盘由用户确认。

**同时回退**：画布"批次化解绑"（用户实测仍会闪，与渲染目标切换次数无关）。

---

## 十六、罗盘止血补丁（根因未找到，勿当通用修复）

`sprite_impl.cc`：纹理名含 `compass` 时额外重画一次（含 pass 边界）。
接缝不受影响（0.098），用户确认罗盘不再丢。

**注意**：这是按素材名的止血，**根因未找到**；其它精灵/其它游戏理论上仍有同样风险。
穷举与排除过程见主文档 §36。

---

## 十七、2026-10-17 白天：当前状态（仪器已重建，结论以主文档 §37 为准）

### ✅ 保留的 3 处真修复

| 位置 | 内容 |
|---|---|
| `content/render/drawable_controller.{h,cc}` + `content/screen/viewport_impl.cc` / `renderscreen_impl.cc` | 遍历时逐级检查**父视口可见性** ⇒ 隐藏视口不再继续绘制里面的窗口（右侧文字闪烁的根因，用户确认已好） |
| `content/screen/renderscreen_impl.cc` | 帧首清除必须在**全屏 scissor 之后**（清除会被 scissor 裁剪） |
| `content/screen/viewport_impl.cc` | 视口开 pass 时清除同样必须在全屏 scissor 之后 |

### ⚠️ 尚未解决（不要当成已修）

- **右下角装饰偶发单帧不显示**：机制收窄为"第一次绘制未落地、第二次相同绘制落地"；`sprredraw=1` 可掩盖，但会让半透明内容二次混合 ⇒ 接缝变深。两者互斥。
- **战斗面板之间的接缝**：`sprredraw=0` 时与参照平台（PJZ110/Adreno）一致。

### 🔧 测量工具（`tools/`，新增）

```bash
bash tools/seam_measure.sh <标签> <秒数>   # 罗盘/接缝闪烁率（唯一可靠的判据）
python tools/seam_metric.py <截图...>     # 接缝"相对深度"，对照参照平台 ≈0.10
python tools/frame_loss.py <帧目录>       # 录屏帧目录里的单帧丢失
```

⚠️ **本机 `screencap` 会返回旧帧**，必须用 `screenrecord` 录屏再抽帧（`640×360` 才能跑到 40fps）。

### 🧹 清理

已删除约 **20 GB** 临时产物与两份源码树副本。剩余 34 个已修改文件里包含约 300 处 `Marker: URGE_*` 探针与已证伪的实验开关（`sprpass` / `cvfreeze` / `cvdefer` / `URGE_RECOMMIT`），**待清理**。

---

## 十八、2026-10-17 傍晚：丢失定位在"合成"这一步

### ✅ 保留的真修复（共 3 处，均经用户确认）

| 位置 | 内容 |
|---|---|
| `content/render/drawable_controller.{h,cc}` + `content/screen/viewport_impl.cc` / `renderscreen_impl.cc` | 遍历时逐级检查**父视口可见性** ⇒ 隐藏视口不再继续绘制里面的窗口（右侧文字闪烁的根因） |
| `content/screen/renderscreen_impl.cc` | 帧首清除必须在**全屏 scissor 之后** |
| `content/screen/viewport_impl.cc` | 视口开 pass 时清除同样必须在全屏 scissor 之后 |

### ⚠️ 待定（默认开，收益未验证）

`content/screen/renderscreen_impl.cc` 的 `URGE_SCREENSTORE`：pass 结束前把 `gpu_.screen_buffer` 显式转成
`RESOURCE_STATE_SHADER_RESOURCE`。动机：`renderscreen_impl.cc` 全文没有任何 `TransitionResourceStates`，
而这张缓冲紧接着就被 ImGui 当作纹理采样。实测瞬态率 3.82% → 3.23%（未解决）。
`urge_debug/screenstore.txt = 0` 可关闭。

### ❌ 已证伪（不要重做，见主文档 §38.4）

| 手段 | 呈现侧瞬态率 |
|---|---|
| 基线 | 3.82% |
| `presentbarrier.txt=1`（呈现前 render scope 边界） | 无改善 |
| `presentsync.txt=1`（合成前 Flush+WaitForIdle） | 3.61% |
| 画布每批次解绑一次 | 无效（已回退） |
| `nobatch` / `sprnodepth` / `cvfreeze` / `keepalive` / `paintdrain` | 无效 |
| `sprredraw.txt=1` | **0** —— 但接缝变深，二者互斥 |

### 🔧 测量

```bash
echo 1 > /storage/emulated/0/Android/data/com.admenri.urge/files/urge_debug/cellprobe.txt
echo 1 > /storage/emulated/0/Android/data/com.admenri.urge/files/urge_debug/diag.txt
# 复现：F 菜单 → 确认两次(scancode 6) → 停在"道具使用界面"
# 然后看 Game.log 里的 [CellProbe] 行，按"中间帧与前后都不同、前后彼此接近"判定
```

⚠️ **本机 `screencap` 会返回旧帧**；引擎内 `PixelProbe` 读的是离屏缓冲（能证明应用画对了，不能证明显示对了）。
唯一能读到"电视上那张图"的仪器是呈现侧 `CellProbe`。

详见主文档 **§38**。

---

## 十九、2026-10-18：分支 `flicker-20261018` 与接手说明

当前工作分支 **`flicker-20261018`**（基于 main，含主干全部清理提交），
与 main 差异 **42 文件 / +6870 −361**。

### 修复确定性

- **A 级（已确认，必留）**：隐藏视口不绘制子窗口；帧首清除在全屏 scissor 之后；视口 pass 清除同样
- **B 级（收益未证实，默认开）**：`screenstore`，瞬态率 3.82%→3.23%，`screenstore.txt=0` 可关
- **C 级（已证伪，勿用）**：见主文档 §38.4
- **D 级（诊断，默认关）**：呈现侧 `cellprobe.txt` 是唯一能观测"电视上那张图"的仪器

### 重测矩阵

| 改动 | 必须重测 |
|---|---|
| sprite/plane/批次 | 接缝 + 罗盘 |
| canvas | 接缝 |
| viewport/drawable/window | 右侧文字 |
| renderscreen 呈现路径、Diligent 子模块 | **全部三项** |
| 只加诊断开关 | 无需重测 |

### 接手前必读

主文档 **§39**（分支总览与交接），包含：设备连接、**绝对路径装机**（相对路径会静默装旧包）、
复现步骤（F 菜单→确认两次→道具使用界面）、**按键 scancode 表**（确认=6/取消=27，
RGSS 的 "Z" 绑的是 D 键）、测量判据、以及 5 条仪器红线。

⚠️ `third_party/DiligentCore` 的 3 个文件工作树改动是**编译必需项**（定义了主仓库引用的
计数器），且**没有 stash 备份**，不要删。

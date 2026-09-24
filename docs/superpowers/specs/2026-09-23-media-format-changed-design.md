# MediaFormatChanged 带内前沿传播设计

日期：2026-09-23
状态：已批准，待实现计划（v2：按代码核对修订挂钩点、原子性、端口名、成功标准）

## 目标

兑现 EVT-4：当 Node 产生新的媒体格式时，`MediaFormatChanged` 必须绑定触发新格式的关键帧，并沿受影响视频输出 Edge 在该关键帧之前到达下游。现有旁路 EventBus 通知继续保留，观察订阅不参与也不阻塞数据面。

## 范围

首期仅实现 `media_format_changed`：

- 任意内置或 so 插件发布 `media_format_changed` 时都可触发带内镜像（两条既有入口统一收敛，见「决策」）。
- 仅处理同一次 `Process` 调用内、`detail.first_key_seq` 有效且与后继视频关键帧 seq 相等的事件。
- 仅注入产生事件 Node 的视频输出 Edge（判定见「数据模型」）。
- 事件作为普通 `GE_PACKET_FLAG_EVENT` Packet 进入 `ProcessRequest.inputs`；现有算子跳过 event 保持兼容。
- 首期消费者是本设计的契约测试；`transcode_controller` 等现有订阅者继续走旁路，不改造。

不在本期：

- 新 C/C++ ABI `on_event` 回调。
- 非视频格式事件、音频格式事件及任意自定义事件的带内传播。
- 异步 completion / Submit 路径携带格式事件（该路径无 InvokeScope 内的同步 emit，可镜像候选天然不存在）。
- 运行期重协商或自动插入转换 Node。
- `EventKind::kDataPlane` 的启用。它是 `ge_event.kind` 域的预留值，与 Packet flag 是两个域；本期带内交付用 `GE_PACKET_FLAG_EVENT` 表达，`kDataPlane` 保持无生产者的显式预留，不再作为本设计的成功标准。

## 当前事实（按源码核对）

- `Packet` 已支持 `GE_PACKET_FLAG_EVENT`（`include/ge/cpp/packet.h:192`）；`EdgeChannel` 与 `InputBinding` 已识别 event 并放入 `InputBatch.events`（`src/input_binding.cpp:169`）。
- **事件有两条互不相交的发布入口**：
  1. 内置算子：`req.events->Publish(...)` → `Scheduler::Sink::Publish`（`src/scheduler_internal.h:45`）→ `on_operator_event` → `Engine::PublishSessionEvent`。
  2. so 插件：`host_api.event_publish` → `HostApiAdapter::EventPublish`（`src/plugin_operator.cpp:377`）→ `SessionServices` → `HostServices.event_publish`（`src/engine.cpp:232`）→ `Engine::PublishSessionEvent`。**不经过 Scheduler::Sink。**
- 当前生产代码里唯一的 `media_format_changed` 生产者是内置算子 `video_encode`（`src/media/video_encode.cpp:188`），detail 携带 `first_key_seq` 字段；它走入口 1。
- `Scheduler::Sink` 同时实现 `EmitSink` 与 `EventSink`，持有 `topo_`/`node_`/`pv_`，并拥有满队列时的 `Park` 落板逻辑（`src/scheduler_internal.h:15-42`）。
- `MoveBatchInto`（`src/scheduler_invoke.cpp:79`）目前为每个 event 填空端口名，下游无法判断 event 来自哪个输入端口。
- Edge 的媒体属性可由 `EdgeChannel::contract().video.has_value()` 判定（`include/ge/cpp/edge_channel.h:66`、`include/ge/cpp/capability.h:183`）。

## 决策

**镜像判定与注入收敛在 `Scheduler::Sink`**，不在 HostApiAdapter：

1. `Sink::Publish` 收到 `media_format_changed` 且 detail 含非零 `first_key_seq` 时，除照常走 `on_operator_event` 旁路外，把候选（seq → detail JSON）暂存在本次 invocation 的 Sink 实例内。Sink 生命周期即一次 `Process` 调用，天然满足"同一调用"边界，无需额外作用域管理。
2. `Sink::Emit` 收到带 `GE_PACKET_FLAG_KEYFRAME`、seq 与某候选相等的 Packet 时，对该输出 Port 的每条**视频** Edge（`r.edge->contract().video.has_value()`），先以与数据包完全相同的 per-edge 控制路径（`HasParked` → `Park` / `PushOne` → `Park`）推入事件 Packet，紧接着推入关键帧。候选命中后移除。
3. 插件路径打通：`HostApiAdapter::InvokeScope` 增持当前调用的 `EventSink*`；`EventPublish` 在存在活跃 InvokeScope 时改经该 `EventSink`（即 Sink::Publish，旁路发布 + 可镜像），无活跃 scope（open/close/async 线程）时维持现有 `SessionServices` 直达路径（仅旁路，不镜像）。任一路径旁路副本恰好一份，无重复发布。

选择该方案的理由：

- 内置与插件生产者被同一挂钩点覆盖；若只挂 HostApiAdapter，则唯一现存生产者 `video_encode`（内置）永远不会被镜像，EVT-4 名义兑现、实际零生效。
- Sink 已经拥有拓扑、路由、Park 状态：事件 Packet 与关键帧共用同一落板机制，满队列时 park 队列内顺序仍是 event→keyframe，不会出现"只成功推一半"的分裂。
- 沿用插件已有 Host API，无 ABI 破坏。
- 下游不支持事件时保持现有跳过行为，不迫使所有 Operator 升级。

替代方案及否决：

1. 挂钩 HostApiAdapter 延迟镜像（v1 方案）：覆盖不到内置算子（见上），且适配器需反向调用路由器，跨层。
2. 仅在 `video_encode` 手工构造事件 Packet：改动小，但插件与其他格式生产者绕开契约。
3. 新增 `on_event` ABI 回调：消费语义更显式，但需要 ABI 版本、插件兼容与完整生命周期设计；当前没有足够事件类型证明其复杂度。

## 数据模型

- 带内事件 Packet：`header.flags` 仅含 `GE_PACKET_FLAG_EVENT`；`header.seq`、`pts_ns`、`topology_version`、`parameter_version` 复制绑定关键帧；`type_tag` 为事件类型标签（interning `media_format_changed`）；metadata 为生产者发布的同一份 detail JSON（含 `first_key_seq`、格式字段），不另造第二份格式描述。
- 候选匹配键写死为 detail 中的 `first_key_seq`（与 `video_encode.cpp:187` 现有字段一致）。缺失、非数字或为 0 → 不暂存，仅旁路。
- 候选存储：Sink 内 `seq → detail` 小表（单次调用内事件数极少，vector 即可）。invocation 结束仍未命中的候选随 Sink 丢弃，同时递增未镜像计数并发一条诊断审计。
- 视频 Edge 判定：`EdgeChannel::contract().video.has_value()`。非视频 Edge（音频/tensor/自定义）跳过注入，但**不**取消该候选对其余视频 Edge 的注入。
- **event 的输入端口归属**：`InputBatch` 增加与 `events` 平行的 `event_ports`；`InputBinding::SkimHead` 摘取 event 时记录所在端口（`b.port`），`MoveBatchInto` 填真实端口名而非空串。多视频输入端口的下游由此可以判定事件属于哪条流。

## 数据流

```text
内置 Node ── req.events->Publish("media_format_changed", {first_key_seq=N,...})
so 插件  ── host_api.event_publish ──(活跃 InvokeScope)──> 同一 EventSink
                        │
                        ▼
            Scheduler::Sink::Publish
              ├─> on_operator_event ──> EventBus 旁路订阅者（恰一份）
              └─> pending[N] = detail

同一 Process 调用内
  Sink::Emit(keyframe seq=N, port P)
    └─ 对 RoutesFor(node, P) 中每条 video Edge：
         ①（同一 per-edge 控制路径）推 event Packet(seq=N)
         ② 紧接着推 keyframe(seq=N)
       非视频 Edge：只推 keyframe

Edge FIFO ──> InputBinding（event 携带端口名）──> ProcessRequest.inputs
```

## 顺序与失败语义

1. 仅当候选 `first_key_seq` 非零、emit 的 Packet 带 `GE_PACKET_FLAG_KEYFRAME`、且 seq 相等时前插。
2. event 与 keyframe 对每条 Edge 使用**同一条** `HasParked`/`Park`/`PushOne` 路径顺序执行：Edge 已有 park 队列 → 两者依次追加（FIFO 保持）；PushOne 遇 `kWouldBlock` → event 落板后 keyframe 直接排其后。任何情形下单条 Edge 上不会出现 keyframe 先于 event。
3. 无有效 `first_key_seq`、无匹配关键帧、非视频输出、invocation 结束未命中：不产生带内 Packet；旁路发布保持原样；递增"未镜像格式事件"计数并写诊断。
4. 事件 Packet 不参与多输入对齐：不满足 required port、不刷新 `kAligned` 窗口、不作为 `kLatest` 触发（现状 `input_binding.cpp` 已如此）；随下一次 acquire 或 event-only batch 交付。
5. 现有媒体算子继续显式跳过 event；格式感知的新算子可在 inputs 中按 `is_event()` + `event_ports` 识别并重配。
6. 事件不跨拓扑/参数版本：版本字段复制自绑定关键帧；旧拓扑 drain 时遵守其 Edge FIFO。

## 组件边界

| 组件 | 责任 | 不负责 |
|---|---|---|
| `Scheduler::Sink` | 暂存候选、匹配 keyframe、构造事件 Packet、按 per-edge 同路径前插 | 格式重协商、下游重配策略 |
| `HostApiAdapter` | 活跃 InvokeScope 时把插件事件转交当前 EventSink | 任何镜像/匹配判断 |
| `PacketRouter` / `RuntimeTopology` | 维持既有 Push/Prepare 语义 | 判断事件是否可镜像 |
| `InputBinding` | event FIFO 可见性 + 记录 event 源端口名 | 解释媒体格式 detail |
| `EventBus` | 继续投递旁路观察副本（恰一份） | 影响数据面顺序 |
| 媒体 Operator | 默认跳过 event | 自动格式转换 |

## 测试与验收

测试算子放在 `tests/unit/test_operators.h` 风格的自定义算子上（emit 关键帧 + 同 seq 发布事件），**不依赖 `GE_ENABLE_MEDIA`**，默认 Debug 构建即可运行。

1. 单 Edge：`event[N] → keyframe[N]` 顺序断言，event 的 metadata 与旁路 detail 一致。
2. 扇出 N≥3 条视频 Edge，其中一条为容量 1 的 block Edge 且预先塞满：每条 Edge 独立观察到 `event[N] → keyframe[N]`；满 Edge 经落板恢复后顺序不变。
3. 混合输出：同 Port 下挂一条非视频 Edge，断言其只收到 keyframe 不收到 event。
4. 负路径：无 `first_key_seq`、seq 不匹配、非 keyframe emit、跨调用（候选滞留至 invocation 结束）——均不注入；旁路订阅者仍收到事件；未镜像计数递增。
5. 插件路径：经 `HostApiAdapter::EventPublish`（构造活跃 InvokeScope）发布后镜像生效；无 InvokeScope 时仅旁路。
6. 端口归属：多输入下游断言 `event_ports` 携带正确端口名。
7. 版本：Mutation publish/drain 前后事件与关键帧携带所属拓扑版本，迟到事件不进入新拓扑。
8. 兼容：现有 `MediaFormatChanged` EventBus 测试与媒体算子行为不变；event 不计入媒体帧统计（`packets_out` 对 control 包已排除，`runtime_topology.cpp:247`）。

## 风险与缓解

- 双入口统一后插件事件的旁路投递路径改变（SessionServices → Sink::Publish）：断言事件属性（session/node 归属、severity、detail）在两条路径下产出一致的旁路 Event。
- detail 过大：metadata 引用既有 JSON 字符串，不复制 payload；上限沿用 EventBus 现有配置。
- 事件类型扩张：镜像资格只硬编码 `media_format_changed`；未来扩展先定义事件 schema 与消费者，再考虑 ABI v2 回调。

## 成功标准

- EVT-4 从文档承诺变为生产实现：内置与插件两条发布入口都能产生带内前沿事件，并有自动化顺序断言。
- `GE_PACKET_FLAG_EVENT` 在生产路径（而非仅测试）拥有真实生产者。
- 文档明确"旁路观察"与"带内前沿"是同一格式事件的两个独立交付面；`EventKind::kDataPlane` 标注为显式预留、非本期范围。

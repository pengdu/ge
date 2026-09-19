# JSON Schema 正式定义

> 版本策略见 `13-接口与类设计.md` §9。本文定义首期 GraphSpec、MutationPatch、PluginManifest 与 CapabilityDescriptor 的规范字段。

## 1. 通用规则

### 1.1 文档头

所有控制 JSON 文档必须包含：

```json
{
  "kind": "GraphSpec",
  "schema_version": 1,
  "$id": "ge.dev/schema/graph/v1"
}
```

- `kind`：文档类别；必填。
- `schema_version`：该类别的整数 major；必填，首期为 `1`。
- `$id`：可选工具标识，其 major 必须与 `schema_version` 相同。
- 未知字段默认拒绝。仅文档顶层 `allow_unknown_fields: true` 时允许未知字段。

### 1.2 标识

- **外部 Node ID**：调用方提供、Graph 内唯一、稳定字符串，正则：`^[A-Za-z][A-Za-z0-9_.-]{0,127}$`。
- 引擎内部将外部 Node ID 映射为 `NodeId(uint64)`；该映射不写入用户 GraphSpec。
- Edge 使用 `from: "<node_id>.<port>"` 与 `to: "<node_id>.<port>"` 标识。
- `type@version`：算子键，示例 `VideoDecoder@2.0.0`；未带版本仅在引擎配置允许默认版本时接受，生产建议显式版本。

### 1.3 值类型

`options`、`parameters`、`metadata` 使用 JSON 值；引擎只做 Schema 声明的通用校验。透明业务对象可被作为 JSON 值传入，但内部字段语义由 Node 解释。

## 2. GraphSpec

### 2.1 顶层结构

```json
{
  "kind": "GraphSpec",
  "schema_version": 1,
  "$id": "ge.dev/schema/graph/v1",
  "name": "live_transcode",
  "description": "optional human readable text",
  "options": {
    "default_device_id": 0,
    "allow_default_operator_version": false
  },
  "nodes": [],
  "edges": []
}
```

| 字段 | 类型 | 必填 | 说明 |
|---|---|---:|---|
| `name` | string | 是 | Graph 名称，GraphSpec 内唯一标识 |
| `description` | string | 否 | 人类描述 |
| `options` | object | 否 | 图级运行选项 |
| `nodes` | array<NodeSpec> | 是 | 至少一个 Node |
| `edges` | array<EdgeSpec> | 是 | 可为空（仅 Source/Sink 验证由 Node 能力决定） |

### 2.2 NodeSpec

```json
{
  "id": "decoder",
  "operator": "VideoDecoder@2.0.0",
  "executor": {
    "kind": "gpu",
    "device_id": 0
  },
  "parallelism": 1,
  "options": {
    "hardware": "cuda"
  },
  "labels": {
    "role": "public-prefix"
  }
}
```

| 字段 | 类型 | 必填 | 默认 | 说明 |
|---|---|---:|---|---|
| `id` | stable node id | 是 | — | 外部稳定 Node ID |
| `operator` | string | 是 | — | `type@version` |
| `executor.kind` | `auto/cpu/gpu` | 否 | `auto` | 执行器类型 |
| `executor.device_id` | integer | 条件 | — | kind 为 gpu 时可指定；`auto` 不可指定 |
| `parallelism` | integer ≥1 | 否 | 1 | 仅无状态 Node 且能力支持时可大于 1 |
| `options` | object | 否 | `{}` | Node 创建参数 |
| `labels` | object<string,string> | 否 | `{}` | 审计/查询标签，不影响语义 |

### 2.3 EdgeSpec

```json
{
  "id": "decoder-to-scale",
  "from": "decoder.video",
  "to": "scale.in",
  "queue": {
    "capacity": 64,
    "policy": "drop_oldest",
    "max_packet_bytes": 8388608
  },
  "sync": "any",
  "feedback": false
}
```

| 字段 | 类型 | 必填 | 默认 | 说明 |
|---|---|---:|---|---|
| `id` | stable edge id | 否 | 引擎派生 | patch/审计建议显式提供 |
| `from` | endpoint | 是 | — | `<node_id>.<output_port>` |
| `to` | endpoint | 是 | — | `<node_id>.<input_port>` |
| `queue.capacity` | integer ≥1 | 否 | 64 | 有界队列容量 |
| `queue.policy` | `block/drop_oldest/drop_newest` | 否 | `block` | 满队列策略 |
| `queue.max_packet_bytes` | integer >0 | 条件 | — | 无法从 Contract 推导包大小时必填 |
| `sync` | `aligned/latest/any` | 否 | Node 默认 | 输入同步策略；以目标输入 Port 能力为准 |
| `feedback` | boolean | 否 | false | 反馈环 Edge，需图校验允许 |

## 3. MutationPatch

### 3.1 顶层结构

```json
{
  "kind": "MutationPatch",
  "schema_version": 1,
  "$id": "ge.dev/schema/patch/v1",
  "base_topology_version": 42,
  "remove_policy": "drain",
  "actions": []
}
```

| 字段 | 类型 | 必填 | 说明 |
|---|---|---:|---|
| `base_topology_version` | integer | 否 | 乐观并发控制版本；缺失时以提交时当前版本为基准 |
| `remove_policy` | `drain/fast` | 否 | 本 patch 的默认删除策略，默认 `drain` |
| `actions` | array<MutationAction> | 是 | 顺序应用到候选 GraphSpec |

### 3.2 插入连续 Node 链

```json
{
  "type": "insert_chain",
  "edge": "decoder-to-encoder",
  "nodes": [
    {"id": "scale", "operator": "VideoScale@1.0.0", "options": {"width": 1280, "height": 720}},
    {"id": "watermark", "operator": "Watermark@1.0.0", "options": {"image": "logo.png"}}
  ],
  "ports": {
    "chain_input": "in",
    "chain_output": "out"
  }
}
```

- `edge`：被拆分的现有 Edge ID。
- `nodes`：有序、非空 NodeSpec 列表；Node ID 不得与候选图已有 ID 冲突。
- `ports`：可选，链首/尾端口显式映射；缺失时仅允许能力描述中唯一可匹配的输入/输出端口。

### 3.3 删除 Node 或连续链

```json
{
  "type": "remove_chain",
  "nodes": ["scale", "watermark"],
  "mode": "bypass",
  "remove_policy": "drain"
}
```

| 字段 | 说明 |
|---|---|
| `nodes` | 有序连续 Node ID 链；单元素等价于删除单 Node |
| `mode` | `disconnect` 或 `bypass` |
| `remove_policy` | 覆盖 patch 默认值 |

`bypass` 只允许唯一外部输入/输出、内部专属线性链。否则返回 `SHARED_DEPENDENCY`、`CAPABILITY_CONFLICT` 或 `GRAPH_INVALID`。

### 3.4 替换 Node

```json
{
  "type": "replace_node",
  "node": "detector",
  "replacement": {
    "operator": "Detector@2.1.0",
    "options": {"model": "det-v2.plan"}
  },
  "port_mapping": {
    "inputs": {"image": "image"},
    "outputs": {"detections": "detections"}
  },
  "remove_policy": "drain"
}
```

未提供 `port_mapping` 时，所有连接 Port 必须同名匹配。参数迁移规则由 replacement Operator 的 Parameter Schema 声明。

### 3.5 删除分支

```json
{
  "type": "remove_branch",
  "entry_edge": "tee-to-rendition-720p",
  "nodes": ["scale_720p", "encoder_720p", "mux_720p", "sink_720p"],
  "remove_policy": "drain"
}
```

`nodes` 是调用方声明的删除范围；校验器确认范围内 Node/Edge 均为专属依赖，不能隐式扩展到共享 Node。

### 3.6 其他动作

```json
{"type": "add_node", "node": {"id": "n", "operator": "Op@1.0.0"}}
{"type": "remove_node", "node": "n", "mode": "disconnect"}
{"type": "add_edge", "edge": {"from": "a.out", "to": "b.in"}}
{"type": "remove_edge", "edge": "a-to-b"}
{"type": "set_node_options", "node": "encoder", "parameters": {"bitrate": 2500000}}
```

`set_node_options` 仅接受声明支持热更新的参数；其语义为下一 Packet 生效，不创建新拓扑版本。

## 4. PluginManifest

```json
{
  "kind": "PluginManifest",
  "schema_version": 1,
  "$id": "ge.dev/schema/plugin/v1",
  "plugin_id": "com.example.vision",
  "library": "libexample_vision.so",
  "abi": {"major": 1, "minor": 0},
  "build_fingerprint": "clang-18-linux-x86_64",
  "dependencies": [
    {"name": "tensorrt", "version": "8.6"}
  ],
  "resources": ["models/detector.plan"],
  "operators": ["Detector@2.0.0"],
  "max_inference_ms": 100,
  "no_owned_threads": true
}
```

| 字段 | 必填 | 说明 |
|---|---:|---|
| `plugin_id` | 是 | 全局唯一反向域名风格 ID |
| `library` | 是 | 相对 manifest 的裸 so 路径，不允许目录穿越 |
| `abi.major/minor` | 是 | C ABI 版本 |
| `build_fingerprint` | 是 | 构建指纹，用于审计与诊断 |
| `dependencies` | 否 | 宿主依赖名称与版本约束 |
| `resources` | 否 | 相对资源路径；加载时检查存在性与可访问性 |
| `operators` | 是 | 声明 type@version 集合，必须与 descriptor 一致 |
| `max_inference_ms` | 条件 | 存在异步推理 Operator 时必填 |
| `no_owned_threads` | 是 | 首期必须为 true |

## 5. CapabilityDescriptor

Capability 由插件 descriptor 返回，查询 API 可返回同一 JSON：

```json
{
  "kind": "CapabilityDescriptor",
  "schema_version": 1,
  "$id": "ge.dev/schema/capability/v1",
  "operator": "VideoDecoder@2.0.0",
  "description": "Hardware accelerated video decoder",
  "ports": {
    "inputs": [
      {"name": "packet", "type_tag": "EncodedVideo", "required": true, "cardinality": "single"}
    ],
    "outputs": [
      {
        "name": "video",
        "type_tag": "VideoFrame",
        "cardinality": "multi",
        "dynamic_consumers": true,
        "video": {
          "pixel_formats": ["NV12", "P010"],
          "width": {"min": 16, "max": 3840},
          "height": {"min": 16, "max": 2160},
          "fps": {"min": 1, "max": 120}
        },
        "memory": {"kinds": ["cuda_device"], "device_ids": [0]}
      }
    ]
  },
  "execution": {
    "devices": ["gpu"],
    "stateful": true,
    "async": false,
    "max_parallelism": 1,
    "zero_copy": true
  },
  "parameters": {
    "hot_updatable": [],
    "schema_ref": "inline"
  },
  "resources": {
    "gpu_memory_bytes": 67108864,
    "nvdec_sessions": 1
  },
  "events": {
    "emits": ["MediaFormatChanged"],
    "accepts": ["RequestKeyFrame"]
  }
}
```

## 6. 正式 JSON Schema 文件规划

后续接口骨架阶段创建：

```text
schema/v1/graph-spec.schema.json
schema/v1/mutation-patch.schema.json
schema/v1/plugin-manifest.schema.json
schema/v1/capability-descriptor.schema.json
```

每份 Schema 均设置 `additionalProperties: false`；仅顶层检测到 `allow_unknown_fields: true` 时由解析器在 Schema 前进行兼容模式处理。

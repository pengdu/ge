# ADR-0001: Engine 拆分组件通过 Services 结构体注入借用指针

日期：2026-09-23
状态：已采纳（52f379d 落地，后续修订消除两段式初始化）

## 背景

`Engine` 曾以 757 行聚合五个变更动机（插件生命周期、会话准入、watchdog、观测、装配）。拆出 `SessionManager` 与 `PluginLifecycleService` 时必须决定：拆出的组件如何访问 Engine 拥有的共享服务（PluginRegistry、ExecutorPool、AsyncRuntime、ResourceLedger、OperationRegistry、AuditLog、EventBus）。

候选方案：

1. 组件持 `Engine&` 反向引用，需要什么调 Engine 的访问器。
2. 组件持各自所需服务的借用裸指针，聚合在一个 `XxxServices` 结构体里，构造期一次注入；跨组件的策略（事件发布、operator generation）以 `std::function` 注入。
3. 为每个服务定义抽象接口，组件依赖接口（完整依赖倒置）。

## 决策

采用方案 2。每个拆出组件定义自己的 `XxxServices` 结构体（见 `src/session_manager.h`、`src/plugin_lifecycle_service.h`），字段为 Engine 拥有的服务的裸指针加少量 `std::function` 回调。

规则：

- **所有权**：指针一律借用，Engine 拥有一切并保证比组件长寿（组件是 Engine 的 `unique_ptr` 成员，声明在被借服务之后）。
- **无反向引用**：组件头文件不得 include `engine.h`，只依赖契约窄头（`engine_config.h`、`upgrade_report.h`）与下层公开头。Engine 头对组件仅前向声明。
- **构造期完整**：全部依赖经构造函数一次注入，构造函数 `assert` 非空；禁止 setter 式两段初始化（曾有 `set_sessions()`，因构造后存在空指针窗口被移除——构造顺序改为 SessionManager 先于 PluginLifecycleService）。
- **析构禁令**：组件析构函数不得触碰任何借用服务。`~Engine` 在函数体内先 reset `async_`、stop `executor_`，此后成员析构期间这些指针已悬空（`engine.h` 成员声明处有同款注释）。
- **Session 引用纪律**：组件不得跨调用存储 `Session*`；需要跨越并发销毁的路径一律走 `SessionManager::FindShared` / `RefsWithIds` 持 `shared_ptr`。
- **锁序**：唯一合法顺序为 `SessionManager::mutex_ → Session::lease_mutex_ → ResourceLedger::mutex_`；组件自有锁（如 `retire_mutex_`）不得与 `SessionManager::mutex_` 嵌套。哨兵测试：`EngineTest.SessionLedgerAndMetricsPathsDoNotDeadlock`。

## 理由

- 对比方案 1：反向引用制造"组件 ⇄ Engine"概念环，且组件可见面等于 Engine 全部公开面，无法从签名看出真实依赖。
- 对比方案 3：本引擎是单进程装配，服务均为单实现；接口层只为可测性服务，而借指针 + 结构体已可在测试中注入替身（构造真实下层对象成本低）。多实现需求出现前不引入（YAGNI）。
- 与既有惯例一致：`Scheduler` 用 `SchedulerEvents` 回调注入、`PluginRegistry` 用 `PluginRegistryOptions::services`，本决策把该模式推广为拆分标准。

## 后果

- 新组件从 Engine 拆出时照此模板：定义 `XxxServices`、构造注入、assert、禁反向 include。
- 代价：Engine 构造函数承担装配顺序知识（SessionManager 必须先于 PluginLifecycleService）；顺序错误由构造期 assert 立即暴露。
- 服务访问器仍留在 Engine 公开面（metrics_export、media controllers 在用）；若未来收敛，另立 ADR。

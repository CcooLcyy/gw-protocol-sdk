# 配电终端统一运维工具协议层 SDK 接口设计 V1.3 到 V1.4 改动点

## 1. 总体变化

V1.4 在 V1.3 的基础上补齐 20260420 技术方案附录 C 的状态自检码实时上送建模。附录 C 定义的实时自检码报文是终端自发上送事件，核心特征为 `TI=42`、`COT=3`，报文内容包含设备自检故障遥信、故障时标和多个故障码。

本次调整后：

- 新增 `iec_self_check_code_t`，用于表达单个状态自检故障码及其故障码遥测信息体地址。
- 新增 `iec_self_check_event_t`，用于表达一次状态自检码实时上送事件。
- 新增 `on_self_check_event` 回调，协议库在收到并解析附录 C 实时自检码报文后触发。
- 明确状态自检码实时上送不走 `on_point_indication`，也不要求调用方只通过 `on_raw_asdu` 自行拆解。
- 明确状态自检码历史记录文件 `ulog` 继续复用通用文件接口读取，不新增专用读取函数。

## 2. 调整依据

20260420 技术方案附录 C 包含三部分内容：

- 状态自检码表：如 `01XXH` 表示电源系统异常，`02XXH` 表示无线模块异常，`03XXH` 表示一次设备或开关状态异常。
- 状态自检码实时上送报文：`TI=42`、`COT=3`，携带设备自检故障遥信、CP56Time2a 故障时标、故障码个数和故障码数组。
- 状态自检码历史文件：文件名 `ulog`，日志类型 `04`，支持 msg/xml 格式。

其中实时上送报文是复合事件，不是普通单个遥信、遥测或累计量点值；若只通过 `on_point_indication` 拆分，会丢失“同一事件内多个故障码”的关系。历史 `ulog` 文件本质是终端文件，已可由通用文件目录和文件读取接口承载。

## 3. 实时自检事件模型

| 变化项 | V1.3 | V1.4 | 影响 |
| --- | --- | --- | --- |
| 实时自检码事件 | 未结构化建模，只能通过原始 ASDU 旁路兜底 | 新增 `iec_self_check_event_t` 和 `on_self_check_event` | 上层可直接接收结构化自检事件 |
| 故障码数组 | 无专用结构体 | 新增 `iec_self_check_code_t` | 保留每个故障码的原始值和信息体地址 |
| `TI=42/COT=3` 处理 | 文档未说明 | 协议库识别并解析为高层事件 | 不要求上层自行判断类型标识和拆字节 |
| 点表回调关系 | 未明确边界 | 不通过 `on_point_indication` 分拆为普通点表对象 | 避免复合事件语义丢失 |

V1.4 新增事件结构体：

```c
typedef struct iec_self_check_code {
    uint32_t information_object_address;   /* 故障码遥测信息体地址 */
    uint32_t code;                         /* 原始状态自检码, 如 0x0202 */
} iec_self_check_code_t;

typedef struct iec_self_check_event {
    uint16_t common_address;               /* ASDU 公共地址 */
    uint8_t type_id;                       /* 类型标识, 附录 C 固定为 42 */
    uint8_t cause_of_transmission;         /* 传送原因, 附录 C 通常为 3 自发 */
    uint8_t originator_address;            /* 发起者地址 */
    uint32_t fault_signal_address;         /* 设备自检故障遥信点号 */
    uint8_t fault_value;                   /* 1 表示有故障码, 0 表示无故障码或故障消除 */
    uint8_t has_fault_time;                /* 是否携带有效故障时标 */
    iec_timestamp_t fault_time;            /* 最后一个故障码发生时刻 */
    const iec_self_check_code_t *codes;    /* 故障码数组, 当前回调期间有效 */
    uint32_t code_count;                   /* 故障码数量 */
} iec_self_check_event_t;
```

## 4. 回调集合变化

V1.4 在回调集合中新增状态自检码实时上送回调：

```c
typedef void (*iec_on_self_check_event_fn)(
    iec_session_t *session,
    const iec_self_check_event_t *event,
    void *user_context);
```

新增字段：

```c
typedef struct iec_callbacks {
    ...
    iec_on_point_indication_fn on_point_indication;
    iec_on_self_check_event_fn on_self_check_event;
    iec_on_command_result_fn on_command_result;
    ...
} iec_callbacks_t;
```

回调语义：

- 由库内工作线程触发，与其他异步回调保持一致。
- `event` 和 `event->codes` 仅在当前回调期间有效；调用方如需跨线程展示、告警或落库，应立即拷贝。
- 回调只表达协议层解析后的结构化事件，不负责将 `0x0202` 等故障码转换为中文描述。

## 5. 历史文件读取口径

V1.4 不新增 `<prefix>_read_self_check_history` 之类的专用函数。

状态自检码历史记录文件仍按通用文件流程处理：

1. 上层按终端约定、工程配置或目录召唤结果定位 `ulog` 文件。
2. 调用 `<prefix>_list_files` 获取目录视图，或直接调用 `<prefix>_read_file` 读取 `ulog`。
3. 通过 `on_file_data_indication` 聚合 msg/xml 文件内容。
4. 通过 `on_file_operation_result` 判断读取是否完整成功。
5. 上层解析 `logType=04` 的历史自检记录，并完成保存、导出和可视化展示。

## 6. 与现有通道的边界

| 通道 | V1.4 口径 |
| --- | --- |
| `on_point_indication` | 继续承载普通点表对象，如遥信、遥测、累计量、周期上送和变化上送；不承载附录 C 复合自检事件 |
| `on_self_check_event` | 承载附录 C `TI=42/COT=3` 状态自检码实时上送事件 |
| `on_raw_asdu` | 继续作为联调抓包和原始报文观察通道；即使开启旁路，高层自检解析成功后仍应触发 `on_self_check_event` |
| 文件接口 | 承载 `ulog` 历史文件读取，不负责实时自检事件回调 |

## 7. 调用方影响

- 调用方需要在 `<prefix>_create` 阶段注册 `on_self_check_event`，用于接收实时自检故障事件。
- 调用方应在回调内拷贝 `iec_self_check_event_t` 中的故障码数组。
- 调用方负责维护附录 C 自检码到诊断对象、状态描述、状态原因、告警等级和展示文本的业务映射。
- 调用方不应把附录 C 实时自检码报文当作普通点表对象处理。
- 历史 `ulog` 文件读取和解析流程不变，仍通过通用文件接口获取文件内容。

## 8. 发布基线影响

本次文档调整新增回调类型和回调集合字段，但不新增 `<prefix>_...` 导出函数。后续实现落地时需要同步更新 public headers、协议解码实现、回调分发测试和自检码报文测试用例；若 public header 已经对外冻结，应结合 ABI 兼容策略评估 `iec_callbacks_t` 结构体扩展方式。

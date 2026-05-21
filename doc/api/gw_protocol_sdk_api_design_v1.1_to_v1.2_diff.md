# 配电终端统一运维工具协议层 SDK 接口设计 V1.1 到 V1.2 改动点

## 1. 总体变化

V1.2 在 V1.1 的基础上收敛了参数下发、自描述文件、文件传输、软件升级、恢复出厂设置和设备重启的接口口径。

主要变化如下：

- 参数下发从单步批量写入调整为预置、执行和取消三个阶段；仍使用 `<prefix>_write_parameters` 一个公开入口，通过请求中的 `mode` 区分阶段。
- 删除 SDK 独立参数校验接口；参数一致性比对、越限检查、差异展示和审计记录由上位机读取实际值后实现。
- 删除终端自描述专用获取接口、专用回调和专用结构体；XML/msg 自描述文件复用通用文件传输接口获取。
- 去除文件传输取消抽象 API 能力；文件传输接口保留目录召唤、文件读取、文件写入、状态查询和断点续传。
- 软件升级调整为统一升级控制接口加显式写文件流程；启动升级、撤销升级和文件升级结束通过 `<prefix>_upgrade_control` 的动作参数区分，升级包写入通过 `<prefix>_write_file` 完成。
- 恢复出厂设置与设备重启通过遥控点表扩展点承载，不新增专用 API；业务流程必须包含遥控预置和遥控执行。
- 恢复出厂设置与设备重启的点号、信息体地址、单双点类型和合闸/执行取值暂不固化，后续以终端自描述、点表模板、工程配置或正式点表为准。

## 2. 参数下发与参数校验

| 变化项 | V1.1 | V1.2 | 影响 |
| --- | --- | --- | --- |
| 参数写入语义 | `<prefix>_write_parameters` 表达批量写入 | `<prefix>_write_parameters` 表达参数下发入口 | 调用方不能再把接口理解为单步写入 |
| 下发阶段 | 未显式建模 | `iec_parameter_write_mode_t` 区分 `PRESET`、`EXECUTE`、`CANCEL` | 上层按预置、执行、取消编排业务 |
| 预置阶段 | 直接提交参数数组并期待写入完成 | `mode = IEC_PARAMETER_WRITE_MODE_PRESET`，携带待预置参数数组 | 预置成功只表示终端接受待下发参数 |
| 执行阶段 | 无单独阶段 | `mode = IEC_PARAMETER_WRITE_MODE_EXECUTE`，参数数组可为空 | 用于使已预置参数生效 |
| 取消阶段 | 无单独阶段 | `mode = IEC_PARAMETER_WRITE_MODE_CANCEL`，参数数组可为空 | 用于撤销尚未执行的预置参数 |
| 参数校验接口 | 提供 `<prefix>_verify_parameters` 和校验请求结构体 | 删除独立参数校验接口 | 参数校验能力迁移到上位机 |
| 参数比对结论 | SDK 可返回一致或不一致结论 | SDK 不生成参数一致性结论 | 上位机读取实际值后自行比对 |

新增或调整的关键类型：

```c
typedef enum iec_parameter_write_mode {
    IEC_PARAMETER_WRITE_MODE_NONE = 0,
    IEC_PARAMETER_WRITE_MODE_PRESET = 1,
    IEC_PARAMETER_WRITE_MODE_EXECUTE = 2,
    IEC_PARAMETER_WRITE_MODE_CANCEL = 3
} iec_parameter_write_mode_t;

typedef struct iec_parameter_write_request {
    uint16_t common_address;
    uint8_t setting_group;
    iec_parameter_write_mode_t mode;
    const iec_parameter_item_t *items;
    uint32_t item_count;
    uint8_t reserved;
} iec_parameter_write_request_t;
```

删除项：

| 删除项 | V1.1 用途 | V1.2 替代方式 |
| --- | --- | --- |
| `<prefix>_verify_parameters` | 由 SDK 发起参数回读校验 | 上位机调用 `<prefix>_read_parameters` 读取实际值后自行比对 |
| `iec_parameter_verify_request_t` | 表达校验请求和期望参数数组 | 期望值由上位机业务模型或参数模板持有 |
| `IEC_PARAMETER_OPERATION_VERIFY` | 标识参数校验操作 | 不再需要 |
| `IEC_PARAMETER_RESULT_VERIFY_OK` | 表示回读一致 | 上位机自行生成业务层比对结论 |
| `IEC_PARAMETER_RESULT_VERIFY_MISMATCH` | 表示回读不一致 | 上位机自行生成业务层差异结果 |

V1.2 推荐流程：

1. 调用 `<prefix>_write_parameters`，`mode = IEC_PARAMETER_WRITE_MODE_PRESET`，携带参数数组。
2. 收到预置成功后，根据业务确认、权限校核或用户操作决定执行或取消。
3. 执行时调用 `<prefix>_write_parameters`，`mode = IEC_PARAMETER_WRITE_MODE_EXECUTE`。
4. 取消时调用 `<prefix>_write_parameters`，`mode = IEC_PARAMETER_WRITE_MODE_CANCEL`。
5. 执行成功后，如需确认结果，调用 `<prefix>_read_parameters` 读取实际值，并由上位机完成比对。

## 3. 自描述文件获取

| 变化项 | V1.1 | V1.2 | 影响 |
| --- | --- | --- | --- |
| 自描述获取入口 | `<prefix>_get_device_description` 专用接口 | 删除专用接口，复用文件传输接口 | 上层按文件读取流程获取 XML/msg |
| 自描述回调 | `on_device_description` | 删除专用回调，使用 `on_file_data_indication` 和 `on_file_operation_result` | 自描述内容按文件块聚合 |
| 自描述类型 | `iec_device_description_request_t`、`iec_device_description_t` 等专用类型 | 删除专用类型，使用 `iec_file_*` 类型 | 类型模型更统一 |
| 文件定位 | 专用请求中表达格式偏好和最大内容长度 | 上层按终端约定、工程配置或目录召唤结果确定目录和文件名 | 自描述文件路径由上层确定 |

V1.2 推荐流程：

1. 若终端支持目录召唤，上层先调用 `<prefix>_list_files` 定位 XML/msg 自描述文件。
2. 若工程配置已明确目录和文件名，可直接调用 `<prefix>_read_file`。
3. 在 `on_file_data_indication` 中按 `transfer_id` 聚合文件块。
4. 以 `on_file_operation_result` 的读取结果判断是否完整成功。
5. XML/msg 解析、缓存、参数模板构建和界面生成仍由上层应用负责。

## 4. 文件传输接口

| 变化项 | V1.1 | V1.2 | 影响 |
| --- | --- | --- | --- |
| 文件传输取消 API | `<prefix>_cancel_file_transfer` | 删除 | 不再向上层暴露取消文件传输抽象能力 |
| 文件操作枚举 | 包含 `IEC_FILE_OPERATION_CANCEL` | 删除取消操作枚举 | 文件操作只保留目录、读取、写入 |
| 文件状态枚举 | 包含 `IEC_FILE_TRANSFER_STATE_CANCELED` | 删除取消状态 | 状态保留已受理、传输中、已完成、失败 |
| 文件结果码 | 包含 `IEC_FILE_RESULT_CANCELED` | 删除取消结果码 | 失败以拒绝、否定确认、偏移不匹配、超时、协议错误等表达 |
| 文件取消流程章节 | 单独描述文件传输取消流程 | 删除该流程 | 与标准规约未规定读写文件取消流程的事实保持一致 |

V1.2 文件能力保留：

- `<prefix>_list_files`：召唤远端文件目录。
- `<prefix>_read_file`：读取远端文件，支持按偏移断点续传。
- `<prefix>_write_file`：写入远端文件，支持按偏移断点续传。
- `<prefix>_get_file_transfer_status`：查询当前会话内本地状态快照。

异常处理口径：

- 标准规约未定义读文件和写文件过程中的独立取消文件传输流程。
- 文件传输失败、链路异常、升级启动到结束之间超时等场景通过超时、否定确认、协议错误、偏移不匹配、对端拒绝或链路事件处理。
- 文件目录、读取和写入的最终结果仍以 `on_file_operation_result` 为准。

## 5. 软件升级

| 变化项 | V1.1 | V1.2 | 影响 |
| --- | --- | --- | --- |
| 升级入口 | `<prefix>_upgrade_firmware` 启动完整升级状态机 | `<prefix>_upgrade_control` 发送升级控制命令 | 上层显式编排升级控制与写文件 |
| 取消升级入口 | `<prefix>_cancel_upgrade` | `operation = IEC_UPGRADE_OPERATION_CANCEL` | 启动、撤销和结束统一在一个控制接口中 |
| 升级进度回调 | `on_upgrade_progress` | 删除进度回调 | 升级控制结果走 `on_upgrade_result`，写文件结果走 `on_file_operation_result` |
| 升级包写入 | 由升级状态机内部承载 | 上层在启动确认后显式调用 `<prefix>_write_file` | 写文件直接复用文件传输接口 |
| 升级控制动作 | 分散在升级状态机语义内 | `IEC_UPGRADE_OPERATION_START`、`FINISH`、`CANCEL` | 协议库根据动作映射 `TI=211` 的 COT 和 `CTYPE.S/E` |

V1.2 升级控制动作映射：

| V1.2 动作 | 业务含义 | 请求报文 | 确认报文 |
| --- | --- | --- | --- |
| `IEC_UPGRADE_OPERATION_START` | 启动升级 | `TI=211, COT=6, CTYPE.S/E=1` | `TI=211, COT=7, CTYPE.S/E=1` |
| `IEC_UPGRADE_OPERATION_FINISH` | 文件升级结束 | `TI=211, COT=6, CTYPE.S/E=0` | `TI=211, COT=7, CTYPE.S/E=0` |
| `IEC_UPGRADE_OPERATION_CANCEL` | 撤销升级 | `TI=211, COT=8, CTYPE.S/E=0` | `TI=211, COT=9, CTYPE.S/E=0` |

V1.2 推荐流程：

1. 上层完成升级包来源校验、可信验签、散列计算和用户确认。
2. 调用 `<prefix>_upgrade_control`，`operation = IEC_UPGRADE_OPERATION_START`，等待 `on_upgrade_result` 返回启动确认。
3. 启动确认成功后，调用 `<prefix>_write_file` 写入升级包；断点续传沿用文件接口的 `acknowledged_offset` 或 `next_offset`。
4. 写文件最终成功以 `on_file_operation_result` 为准。
5. 写文件成功后调用 `<prefix>_upgrade_control`，`operation = IEC_UPGRADE_OPERATION_FINISH`，等待文件升级结束确认。
6. 如需放弃升级，调用 `<prefix>_upgrade_control`，`operation = IEC_UPGRADE_OPERATION_CANCEL`。

## 6. 恢复出厂设置与设备重启

| 变化项 | V1.1 | V1.2 | 影响 |
| --- | --- | --- | --- |
| 承载方式 | 恢复出厂设置描述为遥控，设备重启描述较分散 | 二者均明确通过遥控点表扩展遥控点承载 | 不新增 `<prefix>_factory_reset` 或 `<prefix>_device_reboot` |
| 点位配置 | 示例或说明容易被理解为已有固定点位 | 明确点位暂不确定 | 点号、信息体地址、单双点类型和合闸/执行取值以后续点表为准 |
| 业务流程 | 可能被理解为直接执行或一次调用自动完成 | 必须包含遥控预置和遥控执行 | 上层应在预置确认成功后显式发起执行 |
| 结果边界 | 命令确认和业务恢复边界不够清晰 | `on_command_result` 只表示遥控预置/执行确认、否认或超时 | 恢复后的总召、参数读取、自描述重读由上层编排 |

V1.2 推荐流程：

1. 从终端自描述、点表模板、工程配置或正式点表中取得恢复出厂设置或设备重启的扩展遥控点配置。
2. 完成用户确认、权限校验、安全闭锁和审计记录。
3. 调用 `<prefix>_control_point`，`mode = IEC_COMMAND_MODE_SELECT`，发起遥控预置。
4. 收到预置命令的 `on_command_result` 且结果为接受后，再调用 `<prefix>_control_point`，`mode = IEC_COMMAND_MODE_EXECUTE`，发起遥控执行。
5. 若终端重启或恢复出厂设置导致链路断开，上层按 `on_link_event` 和普通重连流程处理。
6. 链路恢复后，上层重新总召、读取参数或通过文件接口重新读取自描述文件。

## 7. 对调用方的主要影响

- 参数下发调用方必须设置 `iec_parameter_write_request_t.mode`，并按预置、执行、取消阶段处理业务。
- 调用方不能再依赖 SDK 参数校验接口，应在上层维护期望值和参数模板，并通过参数读取结果完成比对。
- 自描述文件获取调用方应改用通用文件接口，不再注册或依赖 `on_device_description`。
- 文件传输调用方不再调用 `<prefix>_cancel_file_transfer`，文件异常收敛依赖超时、协议错误、链路事件和最终文件结果。
- 软件升级调用方需要显式编排 `<prefix>_upgrade_control(START)`、`<prefix>_write_file` 和 `<prefix>_upgrade_control(FINISH)`。
- 恢复出厂设置和设备重启调用方必须按遥控预置和遥控执行两阶段处理，并从外部点表来源取得点位。

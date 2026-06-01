# 配电终端统一运维工具协议层 SDK 接口设计 V1.2 到 V1.3 改动点

## 1. 总体变化

V1.3 在 V1.2 的基础上收敛文件断点续传口径，将断点续传从“文件读取和文件写入通用能力”调整为“写文件方向能力”。

本次调整后：

- `<prefix>_read_file` 只表达远端文件读取，文件数据通过 `on_file_data_indication` 按块返回。
- `iec_file_read_request_t` 不再包含起始偏移字段。
- `iec_file_data_indication_t.current_offset` 和 `next_offset` 只用于文件拼接、缺口诊断和进度显示，不作为读文件断点续传恢复点。
- `<prefix>_write_file` 继续表达远端文件写入，并支持按已确认偏移续传。
- `iec_file_transfer_status_t.acknowledged_offset` 和 `is_resumable` 的续传语义仅适用于写文件方向。

## 2. 调整依据

20260420 技术方案中，读文件过程只描述读文件激活、确认、数据传输和数据传输确认流程，未明确提供读文件起始偏移、续读查询或读方向断点恢复语义。

技术方案的断点续传详细报文扩展集中在写文件方向，包括写文件激活续传查询和写文件激活确认续传响应。因此 V1.3 将 API 文档中的断点续传能力收敛为写文件方向能力。

## 3. 文件读取接口变化

| 变化项 | V1.2 | V1.3 | 影响 |
| --- | --- | --- | --- |
| `<prefix>_read_file` 功能说明 | 读取远端文件，支持按偏移断点续传 | 读取远端文件，文件数据按块返回 | 调用方不应再按读文件续传能力理解该接口 |
| `iec_file_read_request_t.start_offset` | 读文件请求包含起始偏移 | 删除该字段 | 读文件请求只描述目标文件和期望分块 |
| `on_file_data_indication` | 数据块回调可被理解为支撑断点续传 | 数据块回调只提供数据块位置和读取进度 | `current_offset` 和 `next_offset` 不作为读文件恢复点 |
| 文件读取流程章节 | 文件读取与断点续传流程 | 文件读取流程 | 删除链路中断后按偏移重新读文件的示例 |

V1.3 读文件请求结构体：

```c
typedef struct iec_file_read_request {
    uint16_t common_address;                /* 目标公共地址 */
    const char *directory_name;             /* 文件所在目录 */
    const char *file_name;                  /* 目标文件名 */
    uint32_t max_chunk_size;                /* 期望分块大小 */
    uint32_t expected_file_size;            /* 上层已知的总大小, 0 表示未知 */
} iec_file_read_request_t;
```

## 4. 文件写入接口变化

| 变化项 | V1.2 | V1.3 | 影响 |
| --- | --- | --- | --- |
| `<prefix>_write_file` 功能说明 | 写入远端文件，支持按偏移断点续传 | 写入远端文件，支持按已确认偏移续传 | 续传能力保留在写文件方向 |
| `iec_file_write_request_t.start_offset` | 文件读写统一偏移语义 | 写文件起始偏移，首次写入为 `0`，续传时为终端已确认偏移 | 调用方只在写文件方向使用该字段表达续传 |
| `iec_file_transfer_status_t.acknowledged_offset` | 可被理解为文件读写通用恢复点 | 写文件方向有效，表示终端已确认接收的偏移 | 调用方使用该字段重新提交写文件窗口 |
| `iec_file_transfer_status_t.is_resumable` | 可被理解为文件读写通用续传标志 | 写文件方向有效，表示当前写文件传输是否可续传 | 读文件方向不具备续传语义 |

V1.3 写文件续传处理仍使用同一写文件接口：

```c
if (write_status.is_resumable && write_status.acknowledged_offset < content_size) {
    write_req.start_offset = write_status.acknowledged_offset;
    write_req.content = content + write_status.acknowledged_offset;
    write_req.content_size = content_size - write_status.acknowledged_offset;
    m101_write_file(session, &write_req, &write_transfer_id);
}
```

## 5. 状态和回调语义变化

| 类型或回调 | V1.2 | V1.3 |
| --- | --- | --- |
| `iec_file_data_indication_t.current_offset` | 当前块起始偏移，可被用于续传理解 | 当前数据块位置，仅用于拼接、缺口诊断和进度显示 |
| `iec_file_data_indication_t.next_offset` | 下一个建议偏移，可被用于续传理解 | 下一个数据块位置，仅用于拼接、缺口诊断和进度显示 |
| `iec_file_transfer_status_t.acknowledged_offset` | 文件传输已确认偏移 | 写文件已确认偏移，读文件方向不用于续传 |
| `iec_file_transfer_status_t.is_resumable` | 是否可按当前偏移续传 | 写文件是否可按当前偏移续传 |

## 6. 流程章节变化

| 章节 | V1.2 | V1.3 |
| --- | --- | --- |
| 文件读取流程 | `7.10 文件读取与断点续传流程` | `7.10 文件读取流程` |
| 文件读取示例 | 包含 `start_offset` 和按 `acknowledged_offset` 重读 | 删除读文件起始偏移和重读示例 |
| 文件写入流程 | `7.11 文件写入与断点续传流程` | 保留，明确断点续传仅适用于写文件方向 |
| 程序升级流程 | 续传时可按 `acknowledged_offset` 或 `next_offset` 重新提交写文件窗口 | 续传时按 `acknowledged_offset` 重新提交写文件窗口 |

## 7. 调用方影响

- 读取终端 XML/msg 自描述文件、终端模型文件或普通远端文件时，调用方只按普通文件读取流程处理数据块。
- 调用方不应把读文件数据块中的 `next_offset` 作为链路中断后的续读恢复点。
- 写文件和升级包下发场景继续使用 `iec_file_write_request_t.start_offset`、`iec_file_transfer_status_t.acknowledged_offset` 和 `is_resumable` 完成断点续传。
- 程序升级流程中，升级包写入阶段的续传恢复点只使用写文件确认偏移。

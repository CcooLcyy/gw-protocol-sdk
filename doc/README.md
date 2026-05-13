# doc 目录说明

`doc/` 是本仓库的文档工作区，存放接口设计源文档、外部输入资料、导出件和文档构建临时工作区。

## 目录结构

- `api/`：我方维护的接口设计、SDK 设计和协议层 API 文档。
- `external/`：外部正式提供的原始资料，保留原始文件并纳入 Git 追踪。
- `generated/`：由源文档导出的 PDF 等生成件。
- `scripts/`：文档构建、导出和校验脚本。
- `tools/pdf/`：PDF 导出链路使用的样式文件和渲染配置。
- `tmp/`：文档构建中间目录，由脚本自动创建，默认不纳入版本控制。
- `文件分类记录.md`：`doc/` 目录文件分类台账。

## PDF 生成

默认从 `api/gw_protocol_sdk_api_design.md` 生成 PDF 到 `generated/gw_protocol_sdk_api_design.pdf`：

```bash
bash doc/scripts/build_doc_pdf.sh
```

脚本运行时会把 HTML、Mermaid SVG、基础 PDF 等中间文件写入 `doc/tmp/build_pdf/`。

## 维护要求

新增、删除、重命名或替换 `doc/` 下正式文档时，需要同步更新 `文件分类记录.md`。`doc/tmp/` 属于可重建缓存，不需要登记到台账。

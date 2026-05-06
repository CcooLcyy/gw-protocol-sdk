# dtu-unified-maintenance-sdk

配电终端统一运维工具协议层 SDK 仓库，用于沉淀 `IEC 60870-5-101` / `IEC 60870-5-104` 跨平台动态库接口设计、头文件规划、示例和可分发文档。

目标动态库产物包括：

- Windows：`.dll`
- Linux：`.so`
- Android：`.so`

## 目录说明

- `doc/`: 文档工作区，包括 Markdown 主稿、配套 Word 文档、导出件、文档台账和构建临时目录。
- `doc/generated/`: 对外分发使用的 PDF 产物。
- `output/`: 历史导出目录；新脚本输出已统一迁移到 `doc/generated/`。
- `scripts/`: 文档处理脚本。
- `tools/pdf/`: PDF 导出链路使用的样式文件。
- `doc/tmp/`: 构建过程中的中间文件目录，默认不纳入版本控制。

## 当前主文档

- Markdown 源文件：`doc/api/dtu_unified_maintenance_sdk_api_design.md`
- 导出 PDF：`doc/generated/dtu_unified_maintenance_sdk_api_design.pdf`

## 快速生成 PDF

默认生成当前主文档的 PDF：

```bash
bash scripts/build_doc_pdf.sh
```

也可以指定输入和输出路径：

```bash
bash scripts/build_doc_pdf.sh doc/api/dtu_unified_maintenance_sdk_api_design.md doc/generated/dtu_unified_maintenance_sdk_api_design.pdf
```

## 依赖

脚本默认依赖以下工具：

- `python3`
- `node` / `npm`
- `pandoc`
- `wkhtmltopdf`

如果希望为 PDF 自动补充书签目录，再安装：

```bash
python3 -m pip install -r requirements-docs.txt
```

未安装 `pypdf` 时，脚本仍会生成 PDF，只是不会附加 PDF 书签。

## 维护建议

- 优先维护 `doc/` 下的 Markdown 主稿，把 PDF 作为可重复生成的发布产物。
- `doc/tmp/` 下内容视为中间文件，不建议手工修改。
- 如果后续还有更多文档，建议继续复用 `scripts/build_doc_pdf.sh`，避免把导出流程散落在临时目录中。

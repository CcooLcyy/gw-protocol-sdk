# 104/101 文档仓库

这个仓库目前用于沉淀 `IEC 60870-5-101` / `IEC 60870-5-104` Linux 动态库接口设计相关文档，重点是主站侧统一接口方案、评审材料和可分发的 PDF 产物。

## 目录说明

- `doc/`: 原始文档，包括 Markdown 主稿和配套 Word 文档。
- `output/pdf/`: 对外分发或评审使用的 PDF 产物。
- `scripts/`: 文档处理脚本。
- `tools/pdf/`: PDF 导出链路使用的样式文件。
- `tmp/`: 构建过程中的中间文件目录，默认不纳入版本控制。

## 当前主文档

- Markdown 源文件：`doc/104_101动态库接口设计.md`
- 导出 PDF：`output/pdf/104_101动态库接口设计.pdf`

## 快速生成 PDF

默认生成当前主文档的 PDF：

```bash
bash scripts/build_doc_pdf.sh
```

也可以指定输入和输出路径：

```bash
bash scripts/build_doc_pdf.sh doc/104_101动态库接口设计.md output/pdf/104_101动态库接口设计.pdf
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
- `tmp/` 下内容视为中间文件，不建议手工修改。
- 如果后续还有更多文档，建议继续复用 `scripts/build_doc_pdf.sh`，避免把导出流程散落在临时目录中。


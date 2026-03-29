# BGPStream Chunk Runner

这个项目的目标是基于 CAIDA `bgpstream` 和底层 `python/download.py`，对一个较长时间范围内的 BGP update 数据做“分段下载、分段处理、分段清理”，从而避免一次性下载整段历史数据导致磁盘占用过大。

当前架构分成两层：

1. Python 下载层
   固定使用 `python/download.py` 负责远端资源发现、断点续传、重试和文件落盘。

2. C++ 处理中层 + 上层处理器
   C++ 中层负责按配置切片、调用下载脚本、遍历 MRT 文件里的 BGP 报文、把报文批量传给上层处理器，并在每个分片完成后清理当前分片文件。
   具体“怎么处理一批报文”由派生处理器决定。

---

## 整体数据流

整体流程如下：

1. 如果根目录存在本地 `config.json`，程序会先读取它；仓库里提交的是 `config.example.json` 模板。随后再用命令行参数覆盖同名配置，最终构造 `Config`。
2. 按 `chunk_size + chunk_unit` 把 `start_date ~ end_date` 切成多个 `ClosedDateRange`。
3. 对每个分片：
   - 用 `python/download.py --dry-run` 发现该分片需要的 update 文件。
   - 实际下载该分片文件。
   - 使用 `bgpstream` 的 `singlefile` 接口遍历该分片所有文件中的 announcement / withdrawal。
   - 中层把报文组装成 `std::vector<BGPMessage>` 批量交给处理器。
   - 输出一次当前累计统计。
   - 删除当前分片对应的本地文件和 `.part` 文件。
4. 所有分片完成后，输出最终累计统计。
5. 每次程序启动只会生成一个 `log/rcd-*` 日志文件。每个分片处理结束后、正常结束时、异常退出时，都会把当前累计统计追加到同一个文件里。

---

## 目录结构

```text
.
├── CMakeLists.txt
├── README.md
├── config.example.json
├── .clangd
├── python/
│   ├── download.py
│   └── main.py
└── cpp/
    ├── include/bgpstream_runner/
    │   ├── types.h
    │   ├── common.h
    │   ├── config_file.h
    │   ├── download_client.h
    │   ├── message_processor.h
    │   ├── chunk_engine.h
    │   └── prefix_as_stats_processor.h
    └── src/
        ├── main.cpp
        ├── common.cpp
        ├── config_file.cpp
        ├── download_client.cpp
        ├── chunk_engine.cpp
        └── prefix_as_stats_processor.cpp
```

---

## 关键文件说明

### 构建与编辑器

- `CMakeLists.txt`
  项目构建入口。强制 out-of-source build，要求使用 `cmake -S . -B build`。
  同时开启 `CMAKE_EXPORT_COMPILE_COMMANDS=ON`，因此会在 `build/compile_commands.json` 生成 clangd 可用的编译数据库。

- `.clangd`
  告诉 clangd 去 `build/` 目录读取 `compile_commands.json`。

- `config.example.json`
  仓库内提交的配置模板。复制成本地 `config.json` 后即可作为实际运行配置文件。用于配置：
  - 起止日期
  - 分片大小
  - 下载线程数
  - 解析线程数
  - 批量处理大小
  - 日志开关
  - 下载条目限制

### Python 下载层

- `python/download.py`
  底层下载器。负责：
  - 查询可下载资源列表
  - 输出 dry-run 结果
  - 下载 update 文件
  - 处理断点续传、重试和进度显示

- `python/main.py`
  早期 Python 版本的统计入口，目前不是主执行路径。当前主路径已经切到 C++。

### C++ 公共类型与工具

- `cpp/include/bgpstream_runner/types.h`
  定义系统的公共数据结构：
  - `Config`
  - `ClosedDateRange`
  - `RangeProcessingStats`
  - `BGPMessageType`
  - `BGPMessage`

- `cpp/include/bgpstream_runner/common.h`
  `cpp/src/common.cpp`
  放通用工具逻辑，包括：
  - 参数解析
  - 时间范围切片
  - shell 命令执行
  - 文件大小统计
  - 进度条显示

- `cpp/include/bgpstream_runner/config_file.h`
  `cpp/src/config_file.cpp`
  负责解析根目录 JSON 配置文件，并把配置项填充到 `Config`。

### C++ 下载适配层

- `cpp/include/bgpstream_runner/download_client.h`
  `cpp/src/download_client.cpp`
  这是 C++ 对 `python/download.py` 的适配层。
  它不直接实现下载，而是负责：
  - 组装 `download.py` 命令行
  - 执行 dry-run，收集目标文件列表
  - 执行实际下载
  - 自动定位 `python/download.py` 路径

### C++ 中层引擎

- `cpp/include/bgpstream_runner/chunk_engine.h`
  `cpp/src/chunk_engine.cpp`
  这是当前系统的核心中层。职责包括：
  - 按配置的分片大小和单位切片运行
  - 管理每个分片的下载、处理、清理生命周期
  - 使用 `bgpstream` 逐文件遍历 announcement / withdrawal
  - 把报文打包成批次后交给处理器
  - 输出分片级和全局累计统计

  这一层是“通用框架层”，不直接决定业务统计逻辑。

### C++ 可扩展处理器接口

- `cpp/include/bgpstream_runner/message_processor.h`
  定义向上的抽象接口：

  - `name()`
    返回处理器名字，用于汇总输出。
  - `handle_messages(const std::vector<BGPMessage>&)`
    批量处理报文。
    这里采用“批量”而不是“逐条虚函数调用”，目的是降低虚调用开销。
  - `print_summary(std::ostream&)`
    输出处理器自己的统计结果。

### 当前的具体业务处理器

- `cpp/include/bgpstream_runner/prefix_as_stats_processor.h`
  `cpp/src/prefix_as_stats_processor.cpp`
  这是现有业务逻辑的派生实现。它会：
  - 只处理 `Announcement`
  - 忽略 `Withdrawal`
  - 对每个前缀维护一个 `set<ASN>`
  - 统计：
    - `usable_update_elements`
    - `unique_prefixes`
    - `prefix_scoped_as_total`

  注意：同一个 ASN 如果出现在不同前缀的 announcement 里，会被分别计入对应前缀。

### 主入口

- `cpp/src/main.cpp`
  主程序入口。
  它负责：
  - 读取命令行参数
  - 创建 `PrefixAsStatsProcessor`
  - 创建 `ChunkEngine`
  - 启动整条处理链
  - 输出最终统计

---

## 为什么这样拆

这样拆分的核心好处是：

- 下载逻辑和处理逻辑解耦
  `download.py` 不需要知道上层怎么统计，C++ 处理器也不需要知道底层怎么下载。

- 中层稳定、上层可扩展
  如果以后你要做别的统计，只需要再写一个新的 `MessageProcessor` 派生类，不需要改下载流程和分片控制逻辑。

- 批量处理减少虚函数开销
  中层不是“每条报文调用一次虚函数”，而是“积累一批 `BGPMessage` 后再调用一次处理器”，更适合大规模数据遍历。

- 分片清理控制磁盘占用
  每个分片处理完成后立刻删除该分片文件，因此总磁盘占用接近“单分片峰值”，不会随着总时间范围线性增长。

---

## 当前默认统计口径

当前派生处理器的统计逻辑是：

- 中层遍历所有 `announcement` 和 `withdrawal`
- 上层 `PrefixAsStatsProcessor` 只对 `announcement` 生效
- 如果 announcement 中没有前缀或没有 AS path，则不会进入 `usable_update_elements`
- `prefix_scoped_as_total` 是“每个前缀下不同 AS 数量”的总和，不是全局唯一 AS 数量

---

## 构建方式

推荐构建命令：

```bash
cmake -S . -B build
cmake --build build
```

构建后主要产物：

- `build/bgpstream_prefix_stats`
- `build/compile_commands.json`

运行过程中还会在仓库根目录下的 `log/` 目录生成文本记录文件：

- `log/rcd-YYYYMMDD-HH:MM`

每次程序运行只会使用其中一个日志文件；如果不同运行恰好落在同一分钟，程序会自动追加短后缀，例如 `log/rcd-20250329-11:58-001`，避免覆盖旧运行的日志。

记录内容会包含：

- 本次记录生成时间
- 当前运行状态（分片完成 / 成功结束 / 异常退出）
- 起止日期
- collector
- 已处理分片数
- 已使用文件数
- announcement / withdrawal / visited_messages 统计
- 当前处理器的业务统计结果，例如 `unique_prefixes`、`prefix_scoped_as_total`

---

## 运行方式

程序默认会尝试读取根目录下的本地 `config.json`。该文件已被 `.gitignore` 忽略，不会被 Git 追踪。仓库里保留一份`config.example.json`作为模板。如果命令行里传了同名参数，命令行参数优先。

建议先从模板创建本地配置：

```bash
cp config.example.json config.json
```

仓库内的默认模板内容如下，适合测试，使用的是“按天切片”：

```json
{
  "start_date": "2025-01-01",
  "end_date": "2026-01-01",
  "project": "routeviews",
  "collector": "route-views.sg",
  "output_dir": "bgpdata",
  "download_workers": 32,
  "parser_workers": 8,
  "message_batch_size": 4096,
  "chunk_size": 1,
  "chunk_unit": "day",
  "limit": -1,
  "log_phase_transitions": true,
  "log_chunk_summary": true,
  "log_final_summary": true
}
```

示例：

```bash
./build/bgpstream_prefix_stats \
  --start-date 2025-11-01 \
  --end-date 2025-12-01 \
  --download-workers 4 \
  --parser-workers 4 \
  --message-batch-size 1024
```

配置文件相关参数：

- `--config PATH`
- `--no-config`

分片相关命令行参数：

- `--chunk-size N`
- `--chunk-unit day|month`

当前 `config.json` 支持的主要字段：

- `start_date`
- `end_date`
- `project`
- `collector`
- `output_dir`
- `download_workers`
- `parser_workers`
- `message_batch_size`
- `chunk_size`
- `chunk_unit`
- `limit`
- `log_phase_transitions`
- `log_chunk_summary`
- `log_final_summary`

各字段含义：

- `start_date`
  统计起始日期，格式为 `YYYY-MM-DD`。程序会从这一天的 `00:00:00 UTC` 开始处理。

- `end_date`
  统计结束日期，格式为 `YYYY-MM-DD`。这是“包含式”的结束日期，程序内部会自动扩展到下一天的 `00:00:00 UTC` 作为结束边界。

- `project`
  传给 `python/download.py` 的 BGP 项目标识，例如 `routeviews`。

- `collector`
  传给 `python/download.py` 的 collector 名称，例如 `route-views.sg`。

- `output_dir`
  下载文件的本地根目录。实际 update 文件会落在类似 `output_dir/project/collector/updates/` 的路径下。

- `download_workers`
  下载阶段的并发线程数。值越大，单分片下载速度通常越快，但也会增加网络和上游服务压力。

- `parser_workers`
  C++ 中层遍历本地 MRT 文件时的并发线程数。通常对应“同时解析多少个文件”。

- `message_batch_size`
  中层交给处理器的单批报文数量。中层会先把报文聚成一个 `std::vector<BGPMessage>`，再调用一次处理器的 `handle_messages()`。

- `chunk_size`
  分片大小数值。它和 `chunk_unit` 一起决定切片粒度。

- `chunk_unit`
  分片单位，支持 `day` 和 `month`。
  例如：
  - `chunk_size = 1`, `chunk_unit = "day"` 表示按天处理
  - `chunk_size = 1`, `chunk_unit = "month"` 表示按月处理
  - `chunk_size = 7`, `chunk_unit = "day"` 表示按 7 天处理

- `limit`
  下载文件数量限制。
  `-1` 表示不限制；正整数表示每次运行最多只处理前 `N` 个匹配文件，通常用于测试。

- `log_phase_transitions`
  是否输出 `download phase`、`process phase`、`cleanup phase` 这类阶段切换日志。

- `log_chunk_summary`
  是否在每个分片处理完成后输出一次当前累计统计。
  开启后，即使程序中途异常退出，终端里也会保留已经完成分片的累计结果。

- `log_final_summary`
  是否在整次运行完成后输出最终累计统计。

推荐用法：

- 本地调试或快速验证时，推荐使用 `chunk_size = 1` 且 `chunk_unit = "day"`。
- 正式跑较长时间范围时，可以改成 `chunk_size = 1` 且 `chunk_unit = "month"`，或者按需要设置更大的天数 / 月数。

当前运行输出里比较重要的通用字段有：

- `processed_chunks`
- `files_used`
- `visited_messages`
- `announcement_messages`
- `withdrawal_messages`
- `skipped_parse_files`

其中业务处理器 `PrefixAsStatsProcessor` 还会额外输出：

- `usable_update_elements`
- `unique_prefixes`
- `prefix_scoped_as_total`

---

## 后续扩展建议

如果后续要扩展新的统计任务，推荐直接新增一个新的处理器类：

1. 新建一个派生类，继承 `MessageProcessor`
2. 在 `handle_messages()` 里实现自己的批量处理逻辑
3. 在 `print_summary()` 里输出自己的统计结果
4. 在 `cpp/src/main.cpp` 里切换成新的处理器实例

这样可以复用现有的分片下载、文件遍历和清理框架，而不需要重复写底层流程。

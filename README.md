# BGPStream Chunk Runner

这个项目的目标是基于 CAIDA `bgpstream` 和底层 `python/download.py`，对一个较长时间范围内的 BGP update 数据做“分段下载、分段处理、缓存复用”，从而避免一次性下载整段历史数据导致磁盘占用过大，同时减少重复实验时的重复下载成本。

当前架构分成两层：

1. Python 下载层
   固定使用 `python/download.py` 负责远端资源发现、断点续传、重试和文件落盘。

2. C++ 处理中层 + 处理器插件
   C++ 中层负责按配置切片、调用下载脚本、遍历 MRT 文件里的 BGP 报文、把报文批量传给上层处理器。
   具体“怎么处理一批报文”由 `MessageProcessor` 插件决定，主程序在运行时动态加载处理器库。

---

## 整体数据流

整体流程如下：

1. 程序启动时会强制读取仓库根目录的 `config.json`；如果文件不存在，会直接报错退出。仓库里提交的是 `config.example.json` 模板。读取完配置后，再用命令行参数覆盖同名字段，最终构造 `Config`。
2. 按 `chunk_size + chunk_unit` 把 `start_date ~ end_date` 切成多个 `ClosedDateRange`。
3. 对每个分片：
   - 用 `python/download.py --dry-run` 发现该分片需要的 update 文件。
   - 先检查这些文件是否已经缓存到本地；如果都在，就直接跳过远端下载。
   - 如果有缺失文件，会先基于当前 dry-run 能拿到的大小信息做缓存预算；当能估算出“当前缓存大小 + 本次预计新增下载字节”超出 `max_cache_size_gb` 时，会按“最旧文件优先”删除旧缓存。如果即使删掉可淘汰文件也放不下当前分片，会直接报错退出。默认不会额外为 dry-run 做远端 `probe size`。
   - 实际下载当前缺失的分片文件。
   - 使用 `bgpstream` 的 `singlefile` 接口遍历该分片所有文件中的 announcement / withdrawal。
   - 中层把报文组装成 `std::vector<BGPMessage>` 批量交给处理器。
   - 输出一次当前累计统计。
   - 保留已经下载的文件，作为后续实验的本地缓存。
4. 所有分片完成后，输出最终累计统计。
5. 每次程序启动只会生成一个 `log/rcd-*` 日志文件。每个分片处理结束后、正常结束时、异常退出时，都会把当前累计统计追加到同一个文件里。

---

## 目录结构

```text
.
├── manage.sh
├── CMakeLists.txt
├── README.md
├── config.example.json
├── .clangd
├── examples/                      # 仓库内置示例插件
├── plugins/                       # 受 Git 跟踪的持久插件目录
│   └── count_prefix_freq/         # 一个插件一个子目录
│       ├── CMakeLists.txt
│       ├── count_prefix_freq.cpp
│       └── render_prefix_frequency_report.py
├── python/
│   ├── download.py
└── cpp/
    ├── include/bgpstream_runner/
    │   ├── types.h
    │   ├── common.h
    │   ├── config_file.h
    │   ├── download_client.h
    │   ├── message_processor.h
    │   ├── plugin_loader.h
    │   ├── processor_plugin_api.h
    │   └── chunk_engine.h
    └── src/
        ├── main.cpp
        ├── common.cpp
        ├── config_file.cpp
        ├── download_client.cpp
        ├── chunk_engine.cpp
        └── plugin_loader.cpp
```

---

## 关键文件说明

### 构建与编辑器

- `CMakeLists.txt`
  项目构建入口。强制 out-of-source build，要求使用 `cmake -S . -B build`。
  同时开启 `CMAKE_EXPORT_COMPILE_COMMANDS=ON`，因此会在 `build/compile_commands.json` 生成 clangd 可用的编译数据库。
  另外还提供了 `bgpstream_add_processor_plugin(...)`，供 `examples/` 和 `plugins/` 里的插件复用。

- `.clangd`
  告诉 clangd 去 `build/` 目录读取 `compile_commands.json`。

- `config.example.json`
  仓库内提交的配置模板。复制成根目录 `config.json` 后即可作为实际运行配置文件。程序启动时必须能读到这个文件。用于配置：
  - 起止日期
  - 分片大小
  - 处理器插件路径
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
  - `finalize()`
    在整个时间范围内的文件都处理完成后调用一次，适合做最终汇总或收尾处理。
    默认实现为空，不要求每个插件都重写。
  - `print_summary(std::ostream&)`
    输出处理器自己的统计结果。

- `cpp/include/bgpstream_runner/processor_plugin_api.h`
  处理器插件导出接口。自定义处理器只要实现 `MessageProcessor`，并导出固定名字的工厂函数，就可以被主程序动态加载。

- `cpp/include/bgpstream_runner/plugin_loader.h`
  `cpp/src/plugin_loader.cpp`
  负责在运行时通过动态库加载处理器插件。
  构建阶段会生成 `build/bgpstream_processor_plugins.tsv` 清单，主程序据此解析当前可用插件。

### 示例插件

- `examples/CMakeLists.txt`
  注册仓库内置的示例插件，便于直接测试插件架构和切换逻辑。

- `examples/example_message_summary_plugin.cpp`
- `examples/example_announcement_counter_plugin.cpp`
- `examples/example_withdrawal_prefix_plugin.cpp`
- `examples/example_origin_asn_plugin.cpp`
  这些文件提供了几个体量很小的示例处理器，便于参考实现方式，也可以直接通过根目录 `config.json` 的 `processor_plugin` 字段切换使用。

### 持久插件

- `plugins/CMakeLists.txt`
  自动扫描 `plugins/<plugin_name>/CMakeLists.txt`，把每个子目录当作一个独立插件接入构建。

- `plugins/count_prefix_freq/`
  当前仓库里一个受版本控制的持久插件示例。它统计各前缀出现频次，并在处理结束后生成 CSV、JSON 和 SVG 统计图。

### 主入口

- `cpp/src/main.cpp`
  主程序入口。
  它负责：
  - 读取命令行参数
  - 动态加载处理器插件
  - 创建 `ChunkEngine`
  - 启动整条处理链
  - 输出最终统计

---

## 为什么这样拆

这样拆分的核心好处是：

- 下载逻辑和处理逻辑解耦
  `download.py` 不需要知道上层怎么统计，C++ 处理器也不需要知道底层怎么下载。

- 中层稳定、上层可扩展
  如果以后你要做别的统计，只需要单独写一个新的 `MessageProcessor` 插件，不需要改下载流程、分片控制逻辑，也不需要改 `main.cpp`。

- 批量处理减少虚函数开销
  中层不是“每条报文调用一次虚函数”，而是“积累一批 `BGPMessage` 后再调用一次处理器”，更适合大规模数据遍历。

- 缓存上限控制磁盘占用
  已下载文件会保留复用；如果缓存超过配置的上限，程序会在下载前按“最旧文件优先”粗略淘汰旧文件。

---

## 当前插件选择规则

- 所有处理器插件都通过 `bgpstream_add_processor_plugin(...)` 注册到构建系统。
- 构建完成后，主程序会读取 `build/bgpstream_processor_plugins.tsv` 来发现可用插件。
- 当前仓库默认会同时注册 `examples/` 和 `plugins/` 里的多个插件，因此通常需要在根目录 `config.json` 的 `processor_plugin` 字段里显式指定插件名。
- 也可以用 `--processor-plugin NAME_OR_PATH` 临时覆盖根目录 `config.json` 里的同名字段。

---

## 构建方式

推荐构建命令：

```bash
cmake -S . -B build
cmake --build build
```

构建后主要产物：

- `build/bgpstream_prefix_stats`
- `build/bgpstream_processor_plugins.tsv`
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
- 当前处理器的业务统计结果
  具体字段取决于你当前加载的本地插件实现。

缓存目录默认是 `output_dir`。程序不会在每个分片结束后删除缓存文件；如果当前分片需要下载新文件，程序会在下载前尽量评估“当前缓存 + 本次预计新增下载字节”是否超过 `max_cache_size_gb`。超限时会按“最旧文件优先”淘汰旧缓存；如果当前分片本身就无法放进缓存上限，也会直接报错。默认不会额外为 dry-run 做远端 `probe size`，所以在大小未知时，清理判断会退化成只基于当前缓存大小。

根目录下的 [manage.sh](/home/fishtofu/bgpstream/manage.sh) 可以统一执行构建和缓存管理：

```bash
./manage.sh build
./manage.sh run
./manage.sh cache-size
./manage.sh cache-clear
```

其中：

- `build`
  等价于执行 `cmake -S . -B build && cmake --build build`。
- `run`
  先执行一次构建，再启动 `build/bgpstream_prefix_stats`。
- `cache-size`
  读取根目录 `config.json` 里的 `output_dir`，统计当前缓存文件数量和总大小。
- `cache-clear`
  读取根目录 `config.json` 里的 `output_dir`，删除全部缓存文件。

缓存相关命令也支持 `--output-dir PATH` 临时覆盖；`build` 和 `run` 支持 `--build-dir PATH` 指定构建目录。
如果需要给主程序透传参数，可以使用 `--`，例如：

```bash
./manage.sh run -- --start-date 2025-11-01 --end-date 2025-12-01
```

---

## 运行方式

程序启动时会强制读取根目录下的 `config.json`。该文件已被 `.gitignore` 忽略，不会被 Git 追踪。仓库里保留一份 `config.example.json` 作为模板。如果命令行里传了同名参数，命令行参数优先。

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
  "processor_plugin": "example_message_summary_plugin",
  "output_dir": "bgpdata",
  "download_workers": 32,
  "parser_workers": 8,
  "message_batch_size": 1048576,
  "chunk_size": 1,
  "chunk_unit": "day",
  "max_cache_size_gb": 10,
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

配置相关命令行参数：

- `--processor-plugin NAME_OR_PATH`

分片相关命令行参数：

- `--chunk-size N`
- `--chunk-unit day|month`
- `--max-cache-size-gb N`

当前 `config.json` 支持的主要字段：

- `start_date`
- `end_date`
- `project`
- `collector`
- `processor_plugin`
- `output_dir`
- `download_workers`
- `parser_workers`
- `message_batch_size`
- `chunk_size`
- `chunk_unit`
- `max_cache_size_gb`
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

- `processor_plugin`
  处理器插件选择器。可以填写插件名，也可以填写动态库路径。
  如果当前构建里只注册了一个插件，这里可以留空，主程序会自动选择它。
  推荐优先填写插件名，例如 `example_message_summary_plugin`，这样不依赖 `.so` 后缀和绝对路径。
  当前仓库已经注册了多个 `examples/` 示例插件，所以实际使用时应当在根目录 `config.json` 里显式填写这个字段。

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

- `max_cache_size_gb`
  本地缓存目录的大致上限，单位是 `GiB`，支持小数，例如 `1.5`。当当前分片需要下载新文件，而且缓存总量已经明显超过这个值时，程序会在下载前按“最旧文件优先”粗略删除一批旧缓存。

- `limit`
  下载文件数量限制。
  `-1` 表示不限制；正整数表示每次运行最多只处理前 `N` 个匹配文件，通常用于测试。

- `log_phase_transitions`
  是否输出 `download phase`、`process phase`、`cache eviction` 这类阶段切换日志。

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

除此之外，处理器插件还会追加输出自己的业务统计字段，具体由 `print_summary()` 实现决定。

---

## 自定义处理器

如果你想添加自己的 `BGPMessage` 处理器，当前推荐的方式是不改仓库里的主代码，而是在 `plugins/` 目录下为每个插件单独建一个子目录。`plugins/` 受 Git 跟踪，适合放需要长期保留和协作维护的插件。

最小流程如下：

1. 新建 `plugins/my_processor/`
2. 在其中新建 `plugins/my_processor/my_processor.cpp`
3. 在里面继承 `MessageProcessor`
4. 用 `BGPSTREAM_RUNNER_EXPORT_PROCESSOR(...)` 导出工厂函数
5. 新建 `plugins/my_processor/CMakeLists.txt`
6. 在 `plugins/my_processor/CMakeLists.txt` 里调用 `bgpstream_add_processor_plugin(...)`
7. 重新执行 `cmake -S . -B build && cmake --build build`
8. 如果当前只注册了这一个插件，可以直接运行主程序；如果注册了多个插件，就在根目录 `config.json` 里的 `processor_plugin` 字段指定其中一个

一个最小示例：

```cpp
#include "bgpstream_runner/message_processor.h"
#include "bgpstream_runner/processor_plugin_api.h"

class MyProcessor : public bgpstream_runner::MessageProcessor {
   public:
    std::string_view name() const override { return "my_processor"; }

    void handle_messages(const std::vector<bgpstream_runner::BGPMessage> &messages) override {
        processed_ += messages.size();
    }

    void print_summary(std::ostream &out) const override {
        out << "processed_messages: " << processed_ << '\n';
    }

   private:
    std::uint64_t processed_ = 0;
};

BGPSTREAM_RUNNER_EXPORT_PROCESSOR(MyProcessor)
```

对应的 `plugins/my_processor/CMakeLists.txt` 可以写成：

```cmake
bgpstream_add_processor_plugin(
  my_processor_plugin
  ${CMAKE_CURRENT_LIST_DIR}/my_processor.cpp
)
```

构建后，直接把根目录 `config.json` 改成例如：

```json
{
  "processor_plugin": "my_processor_plugin"
}
```

然后运行：

```bash
./build/bgpstream_prefix_stats
```

这样每个插件都有独立的目录，可以自行放置：

- C++ 源文件
- Python 收尾脚本
- 插件自己的 `CMakeLists.txt`
- 插件专用的 README、模板、辅助文件

例如当前的持久插件就是：

- `plugins/count_prefix_freq/CMakeLists.txt`
- `plugins/count_prefix_freq/count_prefix_freq.cpp`
- `plugins/count_prefix_freq/render_prefix_frequency_report.py`

它对应的 `processor_plugin` 取值是：

- `count_prefix_freq_plugin`

根目录 `config.json` 里可以直接这样写：

```json
{
  "processor_plugin": "count_prefix_freq_plugin"
}
```

仓库当前附带的示例插件还有：

- `examples/example_message_summary_plugin.cpp`
  统计处理过的报文数、带前缀的 announcement / withdrawal 数量，以及唯一前缀数。

- `examples/example_announcement_counter_plugin.cpp`
  只统计 announcement 报文数量。

- `examples/example_withdrawal_prefix_plugin.cpp`
  统计带前缀的 withdrawal 报文数量，以及唯一 withdrawn prefix 数量。

- `examples/example_origin_asn_plugin.cpp`
  统计 announcement 中出现过的 origin ASN 数量。

这些示例插件对应的 `processor_plugin` 取值分别是：

- `example_message_summary_plugin`
- `example_announcement_counter_plugin`
- `example_withdrawal_prefix_plugin`
- `example_origin_asn_plugin`

也就是说，根目录 `config.json` 里推荐直接这样写：

```json
{
  "processor_plugin": "example_origin_asn_plugin"
}
```

把这个字段改成不同插件名，就可以在不改 `main.cpp`、不增加额外配置文件的情况下切换处理逻辑。

这样每个持久插件都放在 `plugins/<plugin_name>/` 下，结构清晰，也更适合长期维护；`examples/` 则专门用来放仓库自带的最小示例插件。

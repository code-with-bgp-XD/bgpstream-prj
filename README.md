# BGPStream Chunk Runner

这个项目基于 CAIDA `libBGPStream`，使用 Linux C++17 下载 BGP update MRT 文件，将其一次性预解析成版本化二进制缓存，并让后续分析直接读取解析缓存。下载、预解析和分析均由 C++ 完成，运行时不依赖 Python。

当前架构分成三层：

1. C++ 下载层
   使用 `libcurl` 负责远端资源发现、HTTPS 下载、断点续传、并发、重试和文件落盘。Route Views collector 从官方归档月目录发现实际文件，其他 collector 通过 CAIDA Broker API 发现资源。

2. C++ MRT 预解析与二进制缓存层
   下载模式使用 `libBGPStream` 将每个原始 MRT 文件完整解析一次，按 `(timestamp, timestamp_microseconds)` 和原始读取序号稳定排序，再写入带 schema 版本、源文件指纹、分块校验和的 `.bgpcache` 文件。系统存在 `libzstd.so.1` 时自动使用 Zstd 压缩，否则写入未压缩分块。

3. C++ 分析中层 + 处理器插件
   分析中层按配置切片，优先读取已经生成的 `.bgpcache`，按插件声明的字段投影为 `BGPMessage` 批次并交给上层处理器。默认情况下缓存缺失会终止；开启 `analysis.parse_on_cache_miss` 后，可直接流式解析已经下载的原始 MRT。分析模式始终不会下载文件。
   具体“怎么处理一批报文”由 `MessageProcessor` 插件决定，主程序在运行时动态加载处理器库。

---

## 整体数据流

整体流程如下：

1. 程序启动时强制读取仓库根目录的 `config.json`。配置分为 `analysis` 和 `cache` 两个顶层区块。
2. `./manage.sh download` 使用 `cache` 区块：
   - 发现所需 update 资源；
   - 只下载缺失的原始 MRT，完整原始文件永不由自动缓存淘汰逻辑删除；
   - 检查每个 MRT 对应的解析缓存；缓存缺失、结构损坏、schema 过期或源文件指纹变化时重新生成；
   - 预解析使用磁盘排序临时文件，生成按时间升序稳定排列的 schema v2 缓存；完整结束后原子发布为 `.bgpcache`。
3. `./manage.sh run` 使用 `analysis` 区块：
   - 按 `chunk_size + chunk_unit` 切分日期范围；
   - 通过 `analysis-plan` 进度条显示资源发现与分片路径规划，并在处理消息前确认计划中的原始 MRT 均已下载；
   - 按分片优先读取解析缓存；缓存缺失时根据 `analysis.parse_on_cache_miss` 选择终止，或直接流式解析原始 MRT；
   - 原始 MRT 缺失时无条件终止，分析模式绝不自动下载；已有但损坏或过期的解析缓存也会终止；
   - 从解析缓存或实时 MRT 解析器读取消息，按 `required_message_fields()` 只物化插件需要的字段，再批量交给处理器。
4. 所有分片完成后输出最终累计统计。每次分析运行使用一个 `log/rcd-*` 文件记录分片、成功或异常结果。

缓存目录示例：

```text
bgpdata/routeviews/route-views.sg/updates/
└── 2025/
    └── 01/
        ├── updates.20250101.0000.bz2
        └── parsed-cache/
            └── updates.20250101.0000.bz2.bgpcache
```

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
└── cpp/
    ├── include/bgpstream_runner/
    │   ├── types.h
    │   ├── common.h
    │   ├── config_file.h
    │   ├── download_client.h
    │   ├── mrt_parser.h
    │   ├── parsed_cache.h
    │   ├── message_processor.h
    │   ├── plugin_loader.h
    │   ├── processor_plugin_api.h
    │   └── chunk_engine.h
    └── src/
        ├── main.cpp
        ├── common.cpp
        ├── config_file.cpp
        ├── download_client.cpp
        ├── mrt_parser.cpp
        ├── parsed_cache.cpp
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
  - 解析缓存缺失时是否实时解析原始 MRT
  - 批量处理大小
  - 日志开关
  - 下载条目限制
  - 固定缓存目录和独立的下载/预解析范围

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
  - 文件大小统计
  - 进度条显示（整次任务共用一条总进度，交互式终端中固定在底行，运行日志在上方正常滚动）

- `cpp/include/bgpstream_runner/config_file.h`
  `cpp/src/config_file.cpp`
  负责解析根目录 JSON 配置文件，并把配置项填充到 `Config`。

### BGPMessage 数据模型

`BGPMessage` 可以承载 libBGPStream 公开 record/elem API 能提供的完整信息。预解析阶段保存完整字段集合；分析时插件通过
`required_message_fields()` 声明自己会读取的字段，缓存读取器只把这些字段物化到每条 message 中。字段按来源分为：

- record 级信息：`record_type`、`record_status`、`timestamp`、`timestamp_microseconds`、`project_name`、`collector_name`、`router_name`、`router_ip`、`dump_position`、`dump_timestamp`、`source_file`、`record_index`、`element_index`。
- elem 级来源信息：`type`、`originated_timestamp`、`originated_timestamp_microseconds`、`peer_ip`、`peer_asn`、`prefix`、`next_hop`。
- AS 路径：规范化字符串 `as_path`、保留段类型和边界的 `as_path_segments`、扁平视图 `asns`、可直接使用的 `origin_asn`。
- BGP 路径属性：结构化 `communities`、`origin`、`med`、`local_pref`、`atomic_aggregate`、`aggregator`。
- 状态与注解：`old_peer_state`、`new_peer_state`、`annotations`。

`BGPMessageType` 支持 `RIB`、`Announcement`、`Withdrawal`、`PeerState` 和 `EndOfRib`。当前下载流程仍读取 update 文件，因此通常收到 announcement、withdrawal 和 peer-state；RIB 类型为以后接入 RIB 文件保留。`EndOfRib` 会在所用 libBGPStream 版本公开该元素类型时自动启用。

字段声明和有效性约定：

- `BGPMessageFields` 是按位组合的字段集合。插件必须实现 `required_message_fields()`；未声明字段保持默认值，
  中层不会为它执行字符串转换、容器填充或来源字符串复制，插件也不应读取它。
- `Timestamp` 同时物化 `timestamp` 和 `timestamp_microseconds`；`OriginatedTimestamp`、`PeerStates` 和
  `Annotations` 也分别对应同一逻辑数据组。严格时序插件会由框架自动加入 `Timestamp` 作为顺序校验依赖。
- `ASPathString`、`ASPathSegments`、`FlattenedAsns` 和 `OriginAsn` 是四个独立需求。只声明 `OriginAsn` 时，
  中层只读取 origin ASN，不会顺带生成 AS path 字符串、结构化 segment 或扁平 ASN。
- `asns` 仍是方便统计的扁平视图；需要区分 AS_SET、联盟序列和联盟集合时，应使用 `as_path_segments`。
- `HasASPath` 和 `HasCommunities` 分别只物化 `has_as_path` 和 `has_communities`；检查属性是否存在不会触发
  path 字符串转换或 community 容器填充。
- `origin`、`med`、`local_pref`、`aggregator`、peer-state 字段使用 `std::optional` 表示 libBGPStream 是否提供了该值。
- singlefile 接口把 project/collector 标成 `singlefile`；预解析器会用下载配置中的 project/collector 替换该占位值。读取缓存时，`source_file` 使用当前原始 MRT 的绝对路径恢复，因此仓库移动后不会保留旧路径。
- `record_index` 是 record 在 `source_file` 中的零基序号，`element_index` 是 elem 在该 record 中的零基序号；插件可用三者可靠地重新归组同一底层 BGP record 拆出的元素。

这里的“完整”以 libBGPStream 公开的元素模型为边界。预解析阶段仍会把原始 MRT/BGP 报文解析成
`bgpstream_elem_t`；字段声明优化的是从解析缓存到分析期 `BGPMessage` 的数据构造。未通过公开 elem 字段暴露的
原始字节、未知 path attribute、large/extended community 等无法从这一层恢复。`annotations.cfg` 是带有
借用生命周期的不透明配置指针，因此不会传给插件；`has_rpki_config` 只安全地记录它是否存在。

### C++ 原生下载层

- `cpp/include/bgpstream_runner/download_client.h`
  `cpp/src/download_client.cpp`
  `cpp/src/routeviews_archive.h`
  `cpp/src/routeviews_archive.cpp`
  完整实现资源发现和下载，不启动任何外部脚本或子进程。职责包括：
  - 解析 Route Views 官方月目录中的实际归档文件，或调用 CAIDA Broker API 并解析 JSON 资源清单
  - 使用 `libcurl` 完成 HTTPS、重定向、代理、TLS 证书校验和超时控制
  - 按 `cache.download_workers` 创建并发下载线程
  - 用 `.part` 文件处理断点续传、远端大小校验和最终原子改名
  - 对失败文件执行指数退避重试，并输出可操作的错误提示

下载层只会处理缺失文件和下载产生的 `.part`；不会删除已经完整落盘的原始 MRT。

### MRT 预解析与解析缓存层

- `cpp/include/bgpstream_runner/mrt_parser.h`
  `cpp/src/mrt_parser.cpp`
  使用 libBGPStream 完整遍历单个 MRT，把 record/elem 转换为完整的 `BGPMessage` 批次。

- `cpp/include/bgpstream_runner/parsed_cache.h`
  `cpp/src/parsed_cache.cpp`
  负责：
  - 为每个原始 MRT 生成独立 `.bgpcache`；
  - 记录 schema 版本、稳定时间排序标记、原文件大小和修改时间；
  - 将完整消息暂存到 `.bgpcache.sort.part`，只在内存中保留轻量时间/偏移索引，排序后顺序写入最终缓存；
  - 相同秒和微秒的消息按 MRT 原始读取顺序稳定排列；
  - 分块写入、校验和验证以及可选 Zstd 压缩；
  - 使用 `.part` + 原子改名避免把中断文件当成有效缓存；
  - 分析期执行字段投影和时间范围过滤；
  - 并行生成不同 MRT 的解析缓存。

### C++ 中层引擎

- `cpp/include/bgpstream_runner/chunk_engine.h`
  `cpp/src/chunk_engine.cpp`
  这是当前系统的核心中层。职责包括：
  - 按配置的分片大小和单位切片运行
  - 在处理消息前确认计划中的原始文件均已下载
  - 优先读取解析缓存，并按配置决定缓存缺失时是否实时解析原始 MRT
  - 不调用下载器下载文件
  - 逐文件恢复 RIB / announcement / withdrawal / peer-state / End-of-RIB 元素
  - 把报文打包成批次后交给处理器
  - 输出分片级和全局累计统计

  这一层是“通用框架层”，不直接决定业务统计逻辑。

### C++ 可扩展处理器接口

- `cpp/include/bgpstream_runner/message_processor.h`
  定义向上的抽象接口：

  - `name()`
    返回处理器名字，用于汇总输出。
  - `required_message_fields()`
    返回插件实际读取的 `BGPMessageFields` 位集合。该方法是必需接口；多个字段用 `|` 组合，不需要任何字段时
    返回 `BGPMessageFields::None`。
  - `handle_messages(const std::vector<BGPMessage>&)`
    批量处理报文。
    这里采用“批量”而不是“逐条虚函数调用”，目的是降低虚调用开销。
  - `supports_concurrent_message_handling()`
    声明同一个处理器实例是否允许多个解析线程同时进入 `handle_messages()`。
    默认返回 `false`，因此未显式开启的插件仍由框架串行调用。
  - `requires_strict_chronological_order()`
    声明插件是否要求所有传入报文严格按 `timestamp` 和 `timestamp_microseconds` 的非递减顺序排列。
    默认返回 `false`；返回 `true` 时，严格时序优先于并发处理能力。
  - `finalize()`
    在整个时间范围内的文件都处理完成后调用一次，适合做最终汇总或收尾处理。
    默认实现为空，不要求每个插件都重写。
  - `print_summary(std::ostream&)`
    输出处理器自己的统计结果。

- `cpp/include/bgpstream_runner/processor_plugin_api.h`
  处理器插件导出接口。自定义处理器只要实现 `MessageProcessor`，并用 `BGPSTREAM_RUNNER_EXPORT_PROCESSOR(...)` 导出固定名字的版本函数和工厂函数，就可以被主程序动态加载。

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
  这些文件提供了几个体量很小的示例处理器，便于参考实现方式，也可以直接通过根目录 `config.json` 的 `analysis.processor_plugin` 字段切换使用。

### 持久插件

需要长期维护的自定义插件可以放在 `plugins/<plugin_name>/`。当 `plugins/CMakeLists.txt` 存在时，根构建会自动接入该目录；仓库内置的可运行示例位于 `examples/`。

当前的 `plugins/timestamp_order_checker/` 插件会按接收顺序把 message 的秒级和微秒级时间戳写入
`log/message-timestamps-*.log` 独立文件，并分别报告时间戳是否非递减、是否严格递增。构建后的插件选择器是
`timestamp_order_checker_plugin`。

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
  原生 `DownloadClient` 不需要知道上层怎么统计，C++ 处理器也不需要知道底层怎么下载。

- 中层稳定、上层可扩展
  如果以后你要做别的统计，只需要单独写一个新的 `MessageProcessor` 插件，不需要改下载流程、分片控制逻辑，也不需要改 `main.cpp`。

- 批量处理减少虚函数开销
  中层不是“每条报文调用一次虚函数”，而是“积累一批 `BGPMessage` 后再调用一次处理器”，更适合大规模数据遍历。

- 原始数据与派生缓存分离
  原始 MRT 是不可替代的数据源，自动流程不会淘汰它；解析缓存是可验证、可重新生成的派生文件。

---

## 当前插件选择规则

- 所有处理器插件都通过 `bgpstream_add_processor_plugin(...)` 注册到构建系统。
- 构建完成后，主程序会读取 `build/bgpstream_processor_plugins.tsv` 来发现可用插件。
- 当前仓库会注册 `examples/` 里的多个插件；如果添加了 `plugins/`，也会一并注册。因此通常需要在根目录 `config.json` 的 `analysis.processor_plugin` 字段里显式指定插件名。
- 也可以用 `--processor-plugin NAME_OR_PATH` 临时覆盖根目录 `config.json` 里的 `analysis.processor_plugin`。

---

## Linux 依赖安装

项目的直接构建依赖如下：

- 支持 C++17 的 GCC 或 Clang
- CMake 3.16 或更高版本
- GDB：仅使用 VS Code 调试功能时需要
- `libcurl` 开发头文件和库：原生 HTTP/HTTPS 下载
- `ncurses` 开发头文件和库：终端进度显示
- CAIDA `libBGPStream` 开发头文件和库：MRT/BGP 解析
- `libzstd.so.1`：可选运行时依赖；存在时压缩解析缓存，不需要 Zstd 开发头文件
- POSIX Threads 和 `dl`：由常见 Linux C/C++ 工具链提供

Ubuntu / Debian 推荐按以下步骤安装。先安装本项目自身和添加软件源所需的系统包：

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential cmake gdb curl wget ca-certificates gnupg lsb-release \
  libcurl4-openssl-dev libncurses-dev
```

`libcurl4-gnutls-dev` 也可以替代 `libcurl4-openssl-dev`，二者选择其一即可。

然后按 CAIDA 官方方式添加 Wandio 和 CAIDA 软件源并安装 `libBGPStream`：

```bash
curl -1sLf 'https://dl.cloudsmith.io/public/wand/libwandio/cfg/setup/bash.deb.sh' | sudo -E bash

echo "deb https://pkg.caida.org/os/$(lsb_release -si | awk '{print tolower($0)}') $(lsb_release -sc) main" \
  | sudo tee /etc/apt/sources.list.d/caida.list
sudo wget -O /etc/apt/trusted.gpg.d/caida.gpg \
  https://pkg.caida.org/os/ubuntu/keyring.gpg

sudo apt-get update
sudo apt-get install -y bgpstream
```

CAIDA 的完整安装说明见 [Installing libBGPStream](https://bgpstream.caida.org/docs/install/bgpstream)。如果发行版没有可用二进制包，应按该页面先构建 Wandio，再构建 `libBGPStream`。

Fedora / RHEL 系列可先安装本项目和 `libBGPStream` 源码构建所需的基础包：

```bash
sudo dnf install -y \
  gcc gcc-c++ make cmake gdb curl-devel ncurses-devel \
  zlib-devel bzip2-devel librdkafka-devel
```

安装完成后可检查 CMake 所需的头文件和动态库是否可见：

```bash
test -r /usr/include/bgpstream.h
ldconfig -p | grep -E 'libbgpstream|libcurl|libncurses|libzstd'
```

如果 `libBGPStream` 安装在非标准目录，可以在配置时显式指定：

```bash
cmake -S . -B build \
  -DBGPSTREAM_INCLUDE_DIR=/opt/bgpstream/include \
  -DBGPSTREAM_LIBRARY=/opt/bgpstream/lib/libbgpstream.so
```

不需要安装 Python、pip 或任何 Python 包。

---

## 构建方式

推荐构建命令：

```bash
./manage.sh build
```

该命令会明确使用 Debug 模式并输出到 `build/`。Release 构建使用独立目录：

```bash
./manage.sh build-release
```

构建后主要产物：

- `build/bgpstream_analyzer`
- `build/bgpstream_processor_plugins.tsv`
- `build/compile_commands.json`
- `build-release/bgpstream_analyzer`（Release）

### VS Code 一键构建、运行和调试

仓库内的 `.vscode/` 已包含 Debug 和 Release 模式所需配置。用 VS Code 打开仓库根目录后，按提示安装推荐的 C/C++ 和 CMake Tools 扩展，并确认系统中可以执行 `gdb`。程序运行前还需要存在根目录 `config.json`；首次使用时可由模板复制：

```bash
cp config.example.json config.json
```

常用入口如下：

- CMake Tools 的 Build/Run 按钮：默认配置、构建并运行 `build-release/` 中的 Release 版本。
- `Ctrl+Shift+B`：执行默认任务 `CMake: 构建 (Debug)`，自动完成 CMake 配置和构建。
- `终端 -> 运行任务 -> 运行: bgpstream_analyzer`：自动构建后直接运行。
- `终端 -> 运行任务 -> CMake: 构建 (Release)`：自动配置并构建到 `build-release/`。
- `终端 -> 运行任务 -> 运行: bgpstream_analyzer (Release)`：自动构建 Release 版本后直接运行。
- 在“运行和调试”面板选择 `调试: bgpstream_analyzer (GDB)`，按 `F5`：自动构建后启动 GDB 调试。
- 选择同一调试配置后按 `Ctrl+F5`：自动构建后运行，但不附加调试器。

需要向程序传入临时命令行参数时，可编辑 `.vscode/tasks.json` 或 `.vscode/launch.json` 中对应配置的 `args` 数组。所有入口都把仓库根目录设为工作目录，因此可以正确找到 `config.json` 和构建出的处理器插件清单。

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
- 实时解析的原始 MRT 文件数
- announcement / withdrawal / visited_messages 统计
- 当前处理器的业务统计结果
  具体字段取决于你当前加载的本地插件实现。

缓存目录固定为 `cache.output_dir`。分析阶段不会下载或淘汰文件，并会在处理消息前检查原始 MRT 是否全部存在；任一原始文件缺失都会直接失败。程序按分片优先读取 `.bgpcache`：缓存缺失且 `analysis.parse_on_cache_miss=false` 时失败，设为 `true` 时直接解析已有 MRT。实时解析不会生成 `.bgpcache`；已有缓存内容损坏或源文件指纹不匹配仍会失败。下载阶段负责补齐原始文件并检查、生成或更新派生解析缓存。

根目录下的 `manage.sh` 可以统一执行构建和缓存管理：

```bash
./manage.sh build
./manage.sh build-release
./manage.sh run
./manage.sh run-release
./manage.sh download
./manage.sh download-release
./manage.sh cache-size
./manage.sh cache-clear
```

其中：

- `build`
  使用 `-DCMAKE_BUILD_TYPE=Debug` 配置并构建到 `build/`。
- `build-release`
  使用 `-DCMAKE_BUILD_TYPE=Release` 配置并构建到独立的 `build-release/`。
- `run`
  先执行一次 Debug 构建，再启动 `build/bgpstream_analyzer`。
- `run-release`
  先执行一次 Release 构建，再启动 `build-release/bgpstream_analyzer`。
- `download`
  先执行一次 Debug 构建，再按 `config.json` 的 `cache` 区块下载缺失 MRT 并生成解析缓存。
- `download-release`
  与 `download` 行为相同，但使用 `build-release/` 中的 Release 可执行文件。
- `cache-size`
  读取根目录 `config.json` 里的 `cache.output_dir`，统计当前缓存文件数量和总大小。
- `cache-clear`
  只删除 `parsed-cache/` 下由程序生成的 `.bgpcache`、写入临时文件和排序临时文件，保留所有原始 MRT。

`download` 和 `download-release` 不加载处理器插件，也不执行业务分析。它们先复用或下载原始 MRT，再并行生成解析缓存。网络中断或进程停止后会保留下载 `.part`；强制终止预解析可能留下 `.bgpcache.part` 或 `.bgpcache.sort.part`，可由 `cache-clear` 安全清理。完整原始 MRT 不会被删除。结束日期为包含式。

Route Views 模式以官方 `UPDATES/` 月目录列出的文件为准，不再假定每个 15 分钟整刻都存在固定文件名。归档本身缺失的时间片不会被误报成下载失败；类似 `08:14`、`03:51` 的非整刻文件仍会被发现和处理。目录中已经列出但实际下载失败的文件仍按严格模式重试并报告错误。

分析读取每个分块时还会验证解压结果和校验和。如果出现罕见的缓存内容损坏，可执行 `cache-clear` 后重新运行 `download`；该操作只重建派生缓存，不会删除原始 MRT。

`cache.output_dir` 是下载和分析共用的唯一缓存根目录：相对路径固定以仓库根目录为基准，两个 download 命令
都不能临时覆盖该目录，分析程序也始终从这里查找数据。这样只要 `analysis.project + analysis.collector` 与已下载
数据一致、分析日期位于已下载且已预解析的范围内，后续分析就会直接命中缓存。

所有 build、run 和 download 命令都支持 `--build-dir PATH` 指定构建目录。`download` 和
`download-release` 还支持用命令行临时覆盖 `cache` 区块的数据源、日期、并发数和文件数限制，例如：

```bash
./manage.sh download \
  --project ris \
  --collector rrc00 \
  --start-date 2025-11-01 \
  --end-date 2025-11-07 \
  --download-workers 8 \
  --parser-workers 4 \
  --message-batch-size 8192
```

`cache-size` 和 `cache-clear` 仍可使用 `--output-dir PATH` 检查其他目录或只清理其中的派生解析缓存。程序不再按容量上限自动淘汰任何文件，因此批量预解析前应确认磁盘有足够空间。

如果需要给主程序透传参数，可以使用 `--`，例如：

```bash
./manage.sh run -- --start-date 2025-11-01 --end-date 2025-12-01
```

---

## 运行方式

程序启动时会强制读取根目录下的 `config.json`。该文件已被 `.gitignore` 忽略，不会被 Git 追踪。仓库里保留一份 `config.example.json` 作为模板。配置必须包含 `analysis` 和 `cache` 两个顶层区块。如果命令行里传了当前运行模式的同名参数，命令行参数优先；缓存目录是例外，只能由 `cache.output_dir` 确定。

建议先从模板创建本地配置：

```bash
cp config.example.json config.json
```

仓库内的默认模板内容如下，适合测试，使用的是“按天切片”：

```json
{
  "analysis": {
    "start_date": "2025-01-01",
    "end_date": "2026-01-01",
    "project": "routeviews",
    "collector": "route-views.sg",
    "processor_plugin": "example_message_summary_plugin",
    "parser_workers": 8,
    "message_batch_size": 1048576,
    "parse_on_cache_miss": false,
    "chunk_size": 1,
    "chunk_unit": "day",
    "limit": -1,
    "log_phase_transitions": true,
    "log_chunk_summary": true,
    "log_final_summary": true
  },
  "cache": {
    "start_date": "2025-01-01",
    "end_date": "2026-01-01",
    "project": "routeviews",
    "collector": "route-views.sg",
    "output_dir": "bgpdata",
    "download_workers": 32,
    "parser_workers": 8,
    "message_batch_size": 8192,
    "limit": -1
  }
}
```

示例：

```bash
./build/bgpstream_analyzer \
  --start-date 2025-11-01 \
  --end-date 2025-12-01 \
  --parser-workers 4 \
  --message-batch-size 1024
```

配置相关命令行参数：

- `--processor-plugin NAME_OR_PATH`
- `--parse-on-cache-miss true|false`

分片相关命令行参数：

- `--chunk-size N`
- `--chunk-unit day|month`

`analysis` 区块保留已有的数据分析配置：

- `start_date`
- `end_date`
- `project`
- `collector`
- `processor_plugin`
- `parser_workers`
- `message_batch_size`
- `parse_on_cache_miss`
- `chunk_size`
- `chunk_unit`
- `limit`
- `log_phase_transitions`
- `log_chunk_summary`
- `analysis.log_final_summary`

`cache` 区块保存独立的下载与预解析配置：

- `start_date`
- `end_date`
- `project`
- `collector`
- `output_dir`
- `download_workers`
- `parser_workers`
- `message_batch_size`
- `limit`

各字段含义：

- `analysis.start_date` / `cache.start_date`
  统计起始日期，格式为 `YYYY-MM-DD`。程序会从这一天的 `00:00:00 UTC` 开始处理。

- `analysis.end_date` / `cache.end_date`
  统计结束日期，格式为 `YYYY-MM-DD`。这是“包含式”的结束日期，程序内部会自动扩展到下一天的 `00:00:00 UTC` 作为结束边界。

- `analysis.project` / `cache.project`
  BGP 项目标识，例如 `routeviews` 或 `ris`。使用 Broker API 时它会作为资源过滤条件；Route Views 直连模式下本地目录固定归一为 `routeviews`。

- `analysis.collector` / `cache.collector`
  collector 名称，例如 `route-views.sg` 或 `rrc00`。名称以 `route-views` 开头时使用 Route Views 官方归档，其余名称通过 CAIDA Broker API 发现资源。

- `analysis.processor_plugin`
  处理器插件选择器。可以填写插件名，也可以填写动态库路径。
  如果当前构建里只注册了一个插件，这里可以留空，主程序会自动选择它。
  推荐优先填写插件名，例如 `example_message_summary_plugin`，这样不依赖 `.so` 后缀和绝对路径。
  当前仓库已经注册了多个 `examples/` 示例插件，所以实际使用时应当在根目录 `config.json` 里显式填写这个字段。

- `cache.output_dir`
  下载和分析共用的固定本地缓存根目录。相对路径以仓库根目录为基准，实际 update 文件会按 UTC 年月落在类似 `cache.output_dir/project/collector/updates/YYYY/MM/` 的路径下；对应解析缓存位于该月目录的 `parsed-cache/` 子目录。

- `cache.download_workers`
  下载阶段的并发线程数。值越大，单分片下载速度通常越快，但也会增加网络和上游服务压力。

- `cache.parser_workers`
  预解析阶段同时处理的原始 MRT 文件数。每个线程使用独立 libBGPStream 实例，并写入独立缓存文件。

- `cache.message_batch_size`
  单个解析缓存压缩块最多包含的消息数。较大值通常提高压缩率，但会增加预解析和读取时的峰值内存。排序阶段还会为每个正在处理的 MRT 建立磁盘临时文件和轻量内存索引；提高 `parser_workers` 前应同时评估内存与临时磁盘空间。

- `analysis.parser_workers`
  C++ 中层同时处理的输入文件数；通常读取解析缓存，启用实时回退后也可能直接解析原始 MRT。当插件的 `supports_concurrent_message_handling()` 返回 `true` 时，这些线程也可以同时进入同一个插件实例的 `handle_messages()`；否则框架仍会把插件调用串行化。当 `requires_strict_chronological_order()` 返回 `true` 时，为保证全局时序，框架会忽略这里更大的并发值并只使用一个线程。增加这个线程数也会增加内存占用。

- `analysis.message_batch_size`
  中层交给处理器的单批报文数量。中层会先把报文聚成一个 `std::vector<BGPMessage>`，再调用一次处理器的 `handle_messages()`。

- `analysis.parse_on_cache_miss`
  布尔值，默认 `false`。为 `false` 时，所需 `.bgpcache` 缺失会立即终止分析；为 `true` 时，程序直接用 libBGPStream 流式解析已经下载的原始 MRT，并在本次分析中把消息交给插件，但不会生成解析缓存。此开关不允许自动下载，也不把损坏、过期或读取中失败的已有缓存静默切换为实时解析。实时路径使用 MRT 原始顺序，不执行预解析缓存提供的稳定时间排序；要求严格时序的插件若发现逆序会终止运行。

- `analysis.chunk_size`
  分片大小数值。它和 `chunk_unit` 一起决定切片粒度。

- `analysis.chunk_unit`
  分片单位，支持 `day` 和 `month`。
  例如：
  - `chunk_size = 1`, `chunk_unit = "day"` 表示按天处理
  - `chunk_size = 1`, `chunk_unit = "month"` 表示按月处理
  - `chunk_size = 7`, `chunk_unit = "day"` 表示按 7 天处理

- `analysis.limit` / `cache.limit`
  文件数量限制。`analysis.limit` 限制一次分析最多处理的匹配文件数，`cache.limit` 限制一次下载与预解析最多处理的匹配文件数。`-1` 表示不限制，正整数通常用于测试。

- `analysis.log_phase_transitions`
  是否输出 `plan phase`、`process parsed-cache phase` 或 `process analysis-input phase` 等阶段切换日志。

- `analysis.log_chunk_summary`
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
- `input_mode`
- `parse_on_cache_miss`
- `realtime_parsed_files`
- `processor_concurrent_message_handling`
- `processor_strict_chronological_order`
- `visited_messages`
- `rib_messages`
- `announcement_messages`
- `withdrawal_messages`
- `peer_state_messages`
- `end_of_rib_messages`
- `skipped_parse_files`

`visited_messages` 是上述五类元素计数之和；在当前只读取 update 文件的流程里，`rib_messages` 通常为 0。较旧的 libBGPStream 不产生 End-of-RIB 元素时，`end_of_rib_messages` 也会保持为 0。

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
8. 如果当前只注册了这一个插件，可以直接运行主程序；如果注册了多个插件，就在根目录 `config.json` 里的 `analysis.processor_plugin` 字段指定其中一个

一个最小示例：

```cpp
#include "bgpstream_runner/message_processor.h"
#include "bgpstream_runner/processor_plugin_api.h"

class MyProcessor : public bgpstream_runner::MessageProcessor {
   public:
    std::string_view name() const override { return "my_processor"; }

    bgpstream_runner::BGPMessageFields required_message_fields() const noexcept override {
        return bgpstream_runner::BGPMessageFields::None;
    }

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

上例只使用批次大小，因此声明 `None`。如果处理逻辑读取 `message.type` 和 `message.prefix`，应改为：

```cpp
return bgpstream_runner::BGPMessageFields::Type |
       bgpstream_runner::BGPMessageFields::Prefix;
```

上面的处理器没有重写并发能力声明，因此 `handle_messages()` 保持串行调用。需要注意，串行只表示不会同时进入插件，并不保证多个解析线程提交批次的先后顺序。如果插件已经自行保护所有会在该函数中读写的共享状态，可以显式开启并发：

```cpp
bool supports_concurrent_message_handling() const noexcept override { return true; }
```

开启后，框架不再为这个处理器实例加全局互斥锁；并发调用数最多受 `analysis.parser_workers` 和当前分片文件数限制，调用及完成顺序不作保证。插件必须自行使用原子变量、互斥锁、线程局部状态等方式避免数据竞争。`finalize()` 和 `print_summary()` 只会在当前解析线程全部结束后调用，不会与 `handle_messages()` 并发执行。仓库里的 `example_announcement_counter_plugin` 展示了使用原子计数器安全开启该选项的方式。

如果插件依赖严格的时间顺序，可以添加：

```cpp
bool requires_strict_chronological_order() const noexcept override { return true; }
```

预解析生成的 schema v2 缓存已经按 `(BGPMessage.timestamp, BGPMessage.timestamp_microseconds)` 稳定排序。分析阶段不再执行排序，而是按照资源归档起始时间逐文件读取，并校验缓存内部以及跨批次、跨文件、跨分片的时间戳不发生倒退；相同时间戳的多条报文保持 MRT 中的原始读取顺序。发现顺序违规时，程序会在把乱序消息交给插件之前终止并要求重新生成缓存。严格时序模式始终只使用一个读取线程；即使 `supports_concurrent_message_handling()` 同时返回 `true`，也不会并发进入 `handle_messages()`。

`BGPMessage` 和 `MessageProcessor` 都是插件 ABI 的一部分。字段声明接口把插件 API 提升到了版本 4，旧插件
必须实现 `required_message_fields()` 并用当前头文件重新编译；加载器会拒绝版本不一致的动态库。

对应的 `plugins/my_processor/CMakeLists.txt` 可以写成：

```cmake
bgpstream_add_processor_plugin(
  my_processor_plugin
  ${CMAKE_CURRENT_LIST_DIR}/my_processor.cpp
)
```

构建后，把根目录 `config.json` 中 `analysis` 区块的插件字段改成：

```text
"processor_plugin": "my_processor_plugin"
```

然后运行：

```bash
./build/bgpstream_analyzer
```

这样每个插件都有独立的目录，可以自行放置：

- C++ 源文件
- 插件自己的 `CMakeLists.txt`
- 插件专用的 README、模板、辅助文件

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

也就是说，推荐把根目录 `config.json` 的 `analysis.processor_plugin` 改为：

```text
"processor_plugin": "example_origin_asn_plugin"
```

把这个字段改成不同插件名，就可以在不改 `main.cpp`、不增加额外配置文件的情况下切换处理逻辑。

这样每个持久插件都放在 `plugins/<plugin_name>/` 下，结构清晰，也更适合长期维护；`examples/` 则专门用来放仓库自带的最小示例插件。

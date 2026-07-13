# Timestamp order checker

该插件按接收顺序把每条 message 的 `(timestamp, timestamp_microseconds)` 写入独立日志文件，并统计
相邻 message 之间的时间戳关系。终端和主运行记录只输出汇总，不再与进度条混排。

插件通过 `requires_strict_chronological_order()` 请求框架按时间顺序交付，因此框架会使用单个解析线程，
在批次内稳定排序，并在跨批次、跨文件和跨分片发生时间倒退时直接报错。
插件同时通过 `required_message_fields()` 只声明 `BGPMessageFields::Timestamp`，因此每条 message 不会构造
与时间顺序检查无关的 prefix、地址、AS path 或 community 数据。

构建并运行：

```bash
./manage.sh build
./build/bgpstream_analyzer --processor-plugin timestamp_order_checker_plugin
```

每次运行会创建独立文件：

```text
<仓库根目录>/log/message-timestamps-YYYYMMDD-HHMMSS.log
```

具体路径也会显示在插件汇总的 `timestamp_log_file` 字段中。日志内容示例：

```text
message[0] timestamp=2025-01-01T00:00:00.123456Z epoch=1735689600.123456 order=first
message[1] timestamp=2025-01-01T00:00:00.123456Z epoch=1735689600.123456 order=equal
message[2] timestamp=2025-01-01T00:00:01.000000Z epoch=1735689601.000000 order=forward
```

汇总中的 `timestamps_nondecreasing` 表示是否从未发生时间倒退；相同时间戳是合法的，因为同一个
BGP record 可以产生多条 message。`timestamps_strictly_increasing` 只有在既无倒退、也无相同时间戳时
才为 `true`。

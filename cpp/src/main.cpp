#include "bgpstream_runner/chunk_engine.h"
#include "bgpstream_runner/common.h"
#include "bgpstream_runner/prefix_as_stats_processor.h"

#include <exception>
#include <iostream>
#include <stdexcept>

int main(int argc, char **argv) {
  try {
    const bgpstream_runner::Config config =
        bgpstream_runner::parse_args(argc, argv);
    bgpstream_runner::PrefixAsStatsProcessor processor;
    bgpstream_runner::ChunkEngine engine(config, processor);

    const bgpstream_runner::RangeProcessingStats stats = engine.run();
    if (stats.files_used == 0) {
      throw std::runtime_error("No local files available for statistics.");
    }

    if (config.log_final_summary) {
      engine.print_summary(std::cout, stats, "final cumulative stats");
    }
    return 0;
  } catch (const std::exception &exc) {
    std::cerr << exc.what() << '\n';
    return 1;
  }
}

#include "bgpstream_runner/chunk_engine.h"
#include "bgpstream_runner/common.h"
#include "bgpstream_runner/prefix_as_stats_processor.h"

#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>

int main(int argc, char **argv) {
  std::unique_ptr<bgpstream_runner::PrefixAsStatsProcessor> processor;
  std::unique_ptr<bgpstream_runner::ChunkEngine> engine;
  try {
    const bgpstream_runner::Config config =
        bgpstream_runner::parse_args(argc, argv);
    processor = std::make_unique<bgpstream_runner::PrefixAsStatsProcessor>();
    engine =
        std::make_unique<bgpstream_runner::ChunkEngine>(config, *processor);

    const bgpstream_runner::RangeProcessingStats stats = engine->run();
    if (stats.files_used == 0) {
      throw std::runtime_error("No local files available for statistics.");
    }

    if (config.log_final_summary) {
      engine->print_summary(std::cout, stats, "final cumulative stats");
    }
    engine->write_record_file(stats, "final cumulative stats", "success");
    return 0;
  } catch (const std::exception &exc) {
    if (engine != nullptr) {
      try {
        engine->write_record_file(engine->current_stats(),
                                  "aborted cumulative stats", "failed",
                                  exc.what());
      } catch (const std::exception &record_exc) {
        std::cerr << record_exc.what() << '\n';
      }
    }
    std::cerr << exc.what() << '\n';
    return 1;
  }
}

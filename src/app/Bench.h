#pragma once

#include <string>
#include <vector>

namespace rec {

// Headless capture/convert/encode benchmark: `recordion --bench [options]`. Prints stats to the
// parent console. Used for development, validation and as the PGO training run.
int runBench(const std::vector<std::wstring>& args);

} // namespace rec

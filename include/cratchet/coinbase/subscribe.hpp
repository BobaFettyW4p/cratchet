#pragma once

#include <string>
#include <vector>

namespace cratchet::coinbase {

// The ws-feed subscribe request, as LedgerFlux's ingestor sends it:
// {"type":"subscribe","product_ids":[...],"channels":[...]}
std::string subscribe_message(const std::vector<std::string>& products,
                              const std::vector<std::string>& channels);

}  // namespace cratchet::coinbase

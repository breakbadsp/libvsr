#include "vsr/assert.hpp"
#include "vsr/internal/client_table.hpp"
#include "vsr/internal/layout.hpp"
#include "vsr/message.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *p_data, std::size_t p_size)
{
  const auto input = std::as_bytes(std::span{p_data, p_size});
  const auto message = vsr::DecodeMessage(input);
  static_cast<void>(vsr::DecodeEntry(input));
  static_cast<void>(vsr::PeekMessageBytes(input, std::numeric_limits<std::uint32_t>::max()));
  if (message)
  {
    VSR_ASSERT(message->body_.size() <= input.size());
    VSR_ASSERT(message->body_.data() == input.data() + vsr::kHeaderBytes);
    std::vector<std::byte> encoded(input.size());
    const auto size = vsr::EncodeMessage(message->header_, message->body_, {}, encoded);
    VSR_ASSERT(size == input.size());
    VSR_ASSERT(std::equal(encoded.begin(), encoded.end(), input.begin()));
  }
  vsr::internal::ClientTable table{16, 1024};
  const auto consumed = table.Deserialize(input);
  if (consumed)
  {
    std::vector<std::byte> encoded(4 + 16 * (vsr::internal::kClientEntryBytes + 1024));
    const auto size = table.Serialize(encoded);
    VSR_ASSERT(size == *consumed);
    VSR_ASSERT(std::equal(encoded.begin(), encoded.begin() + static_cast<std::ptrdiff_t>(size),
                          input.begin()));
  }
  return 0;
}

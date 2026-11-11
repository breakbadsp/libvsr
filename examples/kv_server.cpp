#include "kv_state_machine.hpp"
#include "vsr/io/server.hpp"

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Usage: kv_server <replica index> <data file> <address of replica 0> [<address> ...]
// Example, three terminals:
//   kv_server 0 /tmp/kv0.vsr 127.0.0.1:3000 127.0.0.1:3001 127.0.0.1:3002
//   kv_server 1 /tmp/kv1.vsr 127.0.0.1:3000 127.0.0.1:3001 127.0.0.1:3002
//   kv_server 2 /tmp/kv2.vsr 127.0.0.1:3000 127.0.0.1:3001 127.0.0.1:3002
int main(int p_argc, char **p_argv)
{
  constexpr int kFixedArgumentCount = 3;
  if (p_argc <= kFixedArgumentCount)
  {
    std::fputs("usage: kv_server <replica index> <data file> <address>...\n", stderr);
    return EXIT_FAILURE;
  }
  const std::string_view index_text{p_argv[1]};
  unsigned replica = 0;
  const auto parsed =
      std::from_chars(index_text.data(), index_text.data() + index_text.size(), replica);
  const int address_count = p_argc - kFixedArgumentCount;
  if (parsed.ec != std::errc{} || std::cmp_greater_equal(replica, address_count))
  {
    std::fputs("kv_server: the replica index is not valid\n", stderr);
    return EXIT_FAILURE;
  }

  vsr::io::ServerOptions options;
  // Each replica and each client of one cluster uses the same number.
  options.config_.cluster_ = 1;
  options.config_.replica_count_ = static_cast<std::uint8_t>(address_count);
  options.config_.replica_ = static_cast<std::uint8_t>(replica);
  options.data_path_ = p_argv[2];
  for (int index = kFixedArgumentCount; index < p_argc; ++index)
  {
    options.addresses_.emplace_back(p_argv[index]);
  }

  kv::StateMachine state_machine;
  auto server = vsr::io::Server::Create(std::move(options), state_machine);
  if (!server)
  {
    std::fprintf(stderr, "kv_server: %s\n", server.error().c_str());
    return EXIT_FAILURE;
  }
  const auto result = server->Run();
  if (!result)
  {
    std::fprintf(stderr, "kv_server: the replica halted, reason %u\n",
                 static_cast<unsigned>(std::to_underlying(result.error())));
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}

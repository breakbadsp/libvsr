#include "vsr/io/client.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

// Usage: kv_client <operation> <address of replica 0> [<address> ...]
// The operation is "key=value" to set a key, or "key" to read it. The
// output is the value before the operation.
int main(int p_argc, char **p_argv)
{
  constexpr int kFixedArgumentCount = 2;
  if (p_argc <= kFixedArgumentCount)
  {
    std::fputs("usage: kv_client <key>[=<value>] <address>...\n", stderr);
    return EXIT_FAILURE;
  }
  vsr::io::ClientOptions options;
  options.cluster_ = 1;
  for (int index = kFixedArgumentCount; index < p_argc; ++index)
  {
    options.addresses_.emplace_back(p_argv[index]);
  }
  auto client = vsr::io::Client::Create(std::move(options));
  if (!client)
  {
    std::fprintf(stderr, "kv_client: %s\n", client.error().c_str());
    return EXIT_FAILURE;
  }
  const std::string_view operation{p_argv[1]};
  constexpr std::chrono::seconds kTimeout{10};
  const auto reply = client->Request(std::as_bytes(std::span{operation}), kTimeout);
  if (!reply)
  {
    std::fputs("kv_client: no reply. The operation is possibly committed.\n", stderr);
    return EXIT_FAILURE;
  }
  std::fwrite(reply->data(), 1, reply->size(), stdout);
  std::fputc('\n', stdout);
  return EXIT_SUCCESS;
}

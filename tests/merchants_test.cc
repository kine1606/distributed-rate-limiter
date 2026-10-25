// Merchants (Phase 2, P1; src/server/merchants.h): API keys stored hashed, resolved without a lock,
// revoked at the next call; and the service, which refuses a call without a merchant's key and gives
// the engine "<merchant>|<customer>", so two merchants never share a customer.
#include "server/merchants.h"

#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "server/service.h"

namespace ratelimiter {
namespace {

// Answers HGETALL with `fields` (or a Redis error), inline. No script is ever run here.
class FakeHashes final : public RedisBackend {
 public:
  void Eval(const Rule&, std::string_view, std::uint32_t, std::string_view, ScriptCallback done) override {
    done(ScriptResult{.redis_error = true});
  }
  void HGetAll(std::string key, HashCallback done) override {
    HashResult h;
    {
      std::lock_guard lock(mu);
      EXPECT_EQ(key, kMerchantKeysHash);
      h.fields = fields;
      h.redis_error = down;
    }
    done(h);
  }
  void Set(std::unordered_map<std::string, std::string> f) {
    std::lock_guard lock(mu);
    fields = std::move(f);
  }
  std::mutex mu;
  std::unordered_map<std::string, std::string> fields;
  bool down = false;
};

TEST(Merchants, Sha256HexMatchesTheStandardVector) {
  EXPECT_EQ(Sha256Hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(Merchants, MerchantIdsCannotContainTheSeparator) {
  EXPECT_TRUE(ValidMerchant("acme"));
  EXPECT_TRUE(ValidMerchant("Acme_Corp-2"));
  EXPECT_FALSE(ValidMerchant(""));
  EXPECT_FALSE(ValidMerchant("a|b"));
  EXPECT_FALSE(ValidMerchant("a{b}"));
  EXPECT_FALSE(ValidMerchant(std::string(65, 'a')));
  EXPECT_EQ(TenantKey("acme", "cust:42"), "acme|cust:42");
}

TEST(Merchants, ResolvesOnlyKnownKeysAndForgetsRevokedOnes) {
  FakeHashes redis;
  MerchantKeys keys(redis);
  EXPECT_FALSE(keys.Resolve("rl_alpha"));  // nothing loaded yet
  redis.Set({{Sha256Hex("rl_alpha"), "acme"}, {Sha256Hex("rl_beta"), "globex"}, {Sha256Hex("rl_bad"), "bad|id"}});
  ASSERT_TRUE(keys.ReloadAndWait(std::chrono::seconds(1)));
  EXPECT_EQ(keys.Size(), 2u);  // the entry with an invalid merchant id is ignored
  EXPECT_EQ(keys.Resolve("rl_alpha").value_or("-"), "acme");
  EXPECT_EQ(keys.Resolve("rl_alpha").value_or("-"), "acme");  // from this thread's cache
  EXPECT_EQ(keys.Resolve("rl_beta").value_or("-"), "globex");
  EXPECT_FALSE(keys.Resolve("rl_wrong"));
  EXPECT_FALSE(keys.Resolve("rl_bad"));

  redis.Set({{Sha256Hex("rl_beta"), "globex"}});  // acme's key revoked
  ASSERT_TRUE(keys.ReloadAndWait(std::chrono::seconds(1)));
  EXPECT_FALSE(keys.Resolve("rl_alpha"));  // though this thread had it cached
  EXPECT_EQ(keys.Resolve("rl_beta").value_or("-"), "globex");
}

TEST(Merchants, AFailedReloadKeepsTheKeysInUse) {
  FakeHashes redis;
  MerchantKeys keys(redis);
  redis.Set({{Sha256Hex("rl_alpha"), "acme"}});
  ASSERT_TRUE(keys.ReloadAndWait(std::chrono::seconds(1)));
  redis.down = true;
  EXPECT_FALSE(keys.ReloadAndWait(std::chrono::seconds(1)));
  EXPECT_EQ(keys.Resolve("rl_alpha").value_or("-"), "acme");
}

TEST(Merchants, ResolvingWhileTheKeysReloadIsSafe) {  // run under TSan
  FakeHashes redis;
  MerchantKeys keys(redis);
  redis.Set({{Sha256Hex("rl_alpha"), "acme"}});
  ASSERT_TRUE(keys.ReloadAndWait(std::chrono::seconds(1)));
  std::atomic<bool> stop{false};
  std::atomic<int> wrong{0};
  std::vector<std::thread> readers;
  for (int t = 0; t < 4; ++t) {
    readers.emplace_back([&] {
      while (!stop.load()) {
        const auto m = keys.Resolve("rl_alpha");
        if (!m || *m != "acme") wrong.fetch_add(1);
      }
    });
  }
  for (int i = 0; i < 200; ++i) keys.Reload();
  stop = true;
  for (auto& r : readers) r.join();
  EXPECT_EQ(wrong.load(), 0);
}

// Records the keys the engine is asked about.
class RecordingCore final : public RateLimiterCore {
 public:
  void Check(const CheckRequest& request, CheckCallback done) override {
    {
      std::lock_guard lock(mu);
      keys.emplace_back(request.key);
    }
    done(CheckResult{.allowed = true});
  }
  std::mutex mu;
  std::vector<std::string> keys;
};

class TenantServiceTest : public ::testing::Test {
 protected:
  void SetUp() override {
#if defined(__SANITIZE_THREAD__)
    GTEST_SKIP() << "needs a TSan-instrumented gRPC";  // as in service_test.cc
#endif
    redis_.Set({{Sha256Hex("rl_alpha"), "acme"}, {Sha256Hex("rl_beta"), "globex"}});
    ASSERT_TRUE(keys_.ReloadAndWait(std::chrono::seconds(1)));
    grpc::ServerBuilder builder;
    builder.RegisterService(&service_);
    server_ = builder.BuildAndStart();
    ASSERT_TRUE(server_);
    stub_ = v1::RateLimiter::NewStub(server_->InProcessChannel(grpc::ChannelArguments()));
  }
  void TearDown() override {
    if (server_) server_->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(1));
  }
  grpc::Status Check(const std::string& api_key, const std::string& customer) {
    grpc::ClientContext ctx;
    if (!api_key.empty()) ctx.AddMetadata(std::string(kApiKeyHeader), api_key);
    v1::CheckRequest req;
    req.set_key(customer);
    req.set_rule("api_default");
    v1::CheckResponse resp;
    return stub_->Check(&ctx, req, &resp);
  }

  FakeHashes redis_;
  MerchantKeys keys_{redis_};
  std::shared_ptr<RecordingCore> core_ = std::make_shared<RecordingCore>();
  RateLimiterService service_{core_, 10'000, nullptr, &keys_};
  std::unique_ptr<grpc::Server> server_;
  std::unique_ptr<v1::RateLimiter::Stub> stub_;
};

TEST_F(TenantServiceTest, ACallWithoutAKnownKeyIsRefused) {
  EXPECT_EQ(Check("", "cust:1").error_code(), grpc::StatusCode::UNAUTHENTICATED);
  EXPECT_EQ(Check("rl_wrong", "cust:1").error_code(), grpc::StatusCode::UNAUTHENTICATED);
  std::lock_guard lock(core_->mu);
  EXPECT_TRUE(core_->keys.empty());  // refused before the engine
}

TEST_F(TenantServiceTest, EachMerchantsCustomersAreItsOwn) {
  ASSERT_TRUE(Check("rl_alpha", "cust:1").ok());
  ASSERT_TRUE(Check("rl_beta", "cust:1").ok());  // the same id, another merchant's customer
  grpc::ClientContext ctx;
  ctx.AddMetadata(std::string(kApiKeyHeader), "rl_alpha");
  v1::CheckBatchRequest batch;
  for (const char* c : {"cust:2", "cust:3"}) {
    auto* r = batch.add_requests();
    r->set_key(c);
    r->set_rule("api_default");
  }
  v1::CheckBatchResponse resp;
  ASSERT_TRUE(stub_->CheckBatch(&ctx, batch, &resp).ok());
  std::lock_guard lock(core_->mu);
  EXPECT_EQ(core_->keys, (std::vector<std::string>{"acme|cust:1", "globex|cust:1", "acme|cust:2", "acme|cust:3"}));
}

}  // namespace
}  // namespace ratelimiter

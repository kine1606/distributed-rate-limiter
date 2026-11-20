// Merchants (Phase 2, P1 and P3; src/server/merchants.h): API keys stored hashed, resolved without a
// lock, revoked at the next call; each merchant's limits (check rate, capacity and its reserve) as
// rules that stay the same across reloads; and the service, which refuses a call without a merchant's
// key and gives the engine "<merchant>|<customer>" with the merchant's limits, so two merchants never
// share a customer.
#include "server/merchants.h"

#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "server/service.h"

namespace ratelimiter {
namespace {

// Answers HGETALL from `hashes` (or a Redis error), inline. No script is ever run here.
class FakeHashes final : public RedisBackend {
 public:
  using Fields = std::unordered_map<std::string, std::string>;
  void Eval(const Rule&, std::string_view, std::uint32_t, std::string_view, ScriptCallback done) override {
    done(ScriptResult{.redis_error = true});
  }
  void HGetAll(std::string key, HashCallback done) override {
    HashResult h;
    {
      std::lock_guard lock(mu);
      EXPECT_TRUE(key == kMerchantKeysHash || key == kMerchantCapacityHash || key == kMerchantCheckRateHash) << key;
      h.fields = hashes[key];
      h.redis_error = down;
    }
    done(h);
  }
  void Set(Fields keys) { Set(std::string(kMerchantKeysHash), std::move(keys)); }
  void Set(const std::string& hash, Fields f) {
    std::lock_guard lock(mu);
    hashes[hash] = std::move(f);
  }
  std::mutex mu;
  std::unordered_map<std::string, Fields> hashes;
  bool down = false;
};

std::string Who(MerchantKeys& keys, std::string_view api_key) {
  const Merchant* m = keys.Resolve(api_key);
  return m != nullptr ? m->id : "-";
}

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
  EXPECT_EQ(Who(keys, "rl_alpha"), "acme");
  EXPECT_EQ(Who(keys, "rl_alpha"), "acme");  // from this thread's cache
  EXPECT_EQ(Who(keys, "rl_beta"), "globex");
  EXPECT_FALSE(keys.Resolve("rl_wrong"));
  EXPECT_FALSE(keys.Resolve("rl_bad"));

  redis.Set({{Sha256Hex("rl_beta"), "globex"}});  // acme's key revoked
  ASSERT_TRUE(keys.ReloadAndWait(std::chrono::seconds(1)));
  EXPECT_FALSE(keys.Resolve("rl_alpha"));  // though this thread had it cached
  EXPECT_EQ(Who(keys, "rl_beta"), "globex");
}

TEST(Merchants, AFailedReloadKeepsTheKeysInUse) {
  FakeHashes redis;
  MerchantKeys keys(redis);
  redis.Set({{Sha256Hex("rl_alpha"), "acme"}});
  ASSERT_TRUE(keys.ReloadAndWait(std::chrono::seconds(1)));
  redis.down = true;
  EXPECT_FALSE(keys.ReloadAndWait(std::chrono::seconds(1)));
  EXPECT_EQ(Who(keys, "rl_alpha"), "acme");
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
        const Merchant* m = keys.Resolve("rl_alpha");
        if (m == nullptr || m->id != "acme" || m->limits.merchant != "acme") wrong.fetch_add(1);
      }
    });
  }
  for (int i = 0; i < 200; ++i) keys.Reload();
  stop = true;
  for (auto& r : readers) r.join();
  EXPECT_EQ(wrong.load(), 0);
}

TEST(Merchants, BucketTermsAreReadLeniently) {
  const BucketTerms t = ParseBucketTerms("rate 1000 burst 2000 reserve 20");
  EXPECT_EQ((std::tuple{t.rate, t.burst, t.reserve}), (std::tuple{1000u, 2000u, 20u}));
  const BucketTerms one_second = ParseBucketTerms("  rate 50   colour red ");
  EXPECT_EQ((std::tuple{one_second.rate, one_second.burst, one_second.reserve}), (std::tuple{50u, 50u, 0u}));
  const BucketTerms bad = ParseBucketTerms("rate -5 burst 1e3 reserve 400");
  EXPECT_EQ((std::tuple{bad.rate, bad.burst, bad.reserve}), (std::tuple{0u, 0u, 100u}));
  EXPECT_EQ(ParseBucketTerms("").rate, 0u);
}

TEST(Merchants, EachMerchantCarriesItsLimitsAsRules) {
  FakeHashes redis;
  MerchantKeys keys(redis);
  redis.Set({{Sha256Hex("rl_alpha"), "acme"}, {Sha256Hex("rl_beta"), "globex"}, {Sha256Hex("rl_gamma"), "initech"}});
  redis.Set(std::string(kMerchantCapacityHash), {{"acme", "rate 100 burst 200 reserve 20"}, {"initech", "rate 100 reserve 100"}});
  redis.Set(std::string(kMerchantCheckRateHash), {{"acme", "rate 1000"}});
  ASSERT_TRUE(keys.ReloadAndWait(std::chrono::seconds(1)));
  const MerchantLimits& acme = keys.Resolve("rl_alpha")->limits;
  EXPECT_EQ(acme.merchant, "acme");
  ASSERT_TRUE(acme.checks && acme.capacity && acme.reserve);
  EXPECT_EQ((std::tuple{acme.checks->name, acme.checks->limit, acme.checks->refill_per_sec}),
            (std::tuple{std::string("merchant_checks"), 1000u, 1000.0}));
  // 80% of rate and burst for every plan, 20% kept for plans with a fee.
  EXPECT_EQ((std::tuple{acme.capacity->name, acme.capacity->limit, acme.capacity->refill_per_sec}),
            (std::tuple{std::string("merchant_capacity"), 160u, 80.0}));
  EXPECT_EQ((std::tuple{acme.reserve->name, acme.reserve->limit, acme.reserve->refill_per_sec}),
            (std::tuple{std::string("merchant_reserve"), 40u, 20.0}));
  EXPECT_EQ(acme.capacity->algorithm, Algorithm::kTokenBucket);
  const MerchantLimits& globex = keys.Resolve("rl_beta")->limits;  // set nothing: nothing applies
  EXPECT_TRUE(globex.checks == nullptr && globex.capacity == nullptr && globex.reserve == nullptr);
  const MerchantLimits& initech = keys.Resolve("rl_gamma")->limits;  // all of it reserved
  EXPECT_EQ(initech.capacity, nullptr);
  ASSERT_NE(initech.reserve, nullptr);
  EXPECT_EQ(initech.reserve->limit, 100u);
}

TEST(Merchants, LimitRulesStayTheSameAcrossReloads) {  // a lease finds its rule by address
  FakeHashes redis;
  MerchantKeys keys(redis);
  redis.Set({{Sha256Hex("rl_alpha"), "acme"}, {Sha256Hex("rl_beta"), "globex"}});
  redis.Set(std::string(kMerchantCapacityHash), {{"acme", "rate 100"}, {"globex", "rate 100"}});
  ASSERT_TRUE(keys.ReloadAndWait(std::chrono::seconds(1)));
  const Rule* first = keys.Resolve("rl_alpha")->limits.capacity;
  EXPECT_EQ(keys.Resolve("rl_beta")->limits.capacity, first);  // same numbers: one rule, two keys
  EXPECT_EQ(keys.Resolve("rl_alpha")->limits.reserve, nullptr);  // no reserve asked
  ASSERT_TRUE(keys.ReloadAndWait(std::chrono::seconds(1)));
  EXPECT_EQ(keys.Resolve("rl_alpha")->limits.capacity, first);
  redis.Set(std::string(kMerchantCapacityHash), {{"acme", "rate 300"}, {"globex", "rate 100"}});
  ASSERT_TRUE(keys.ReloadAndWait(std::chrono::seconds(1)));
  EXPECT_NE(keys.Resolve("rl_alpha")->limits.capacity, first);
  EXPECT_EQ(keys.Resolve("rl_alpha")->limits.capacity->limit, 300u);
  EXPECT_EQ(keys.Resolve("rl_beta")->limits.capacity, first);
}

// Holds every HGETALL answer, with the hash as it was when asked, until the test releases it.
class DeferredHashes final : public RedisBackend {
 public:
  void Eval(const Rule&, std::string_view, std::uint32_t, std::string_view, ScriptCallback done) override {
    done(ScriptResult{.redis_error = true});
  }
  void HGetAll(std::string key, HashCallback done) override {
    HashResult h;
    h.fields = hashes[key];
    pending.emplace_back([h, done = std::move(done)] { done(h); });
  }
  void Release(std::size_t i) {  // answers the i-th call still pending
    auto answer = std::move(pending[i]);
    pending.erase(pending.begin() + static_cast<std::ptrdiff_t>(i));
    answer();
  }
  std::unordered_map<std::string, FakeHashes::Fields> hashes;
  std::vector<std::function<void()>> pending;
};

TEST(Merchants, AnOlderReloadFinishingLastNeverBringsBackARevokedKey) {
  DeferredHashes redis;
  MerchantKeys keys(redis);
  redis.hashes[std::string(kMerchantKeysHash)] = {{Sha256Hex("rl_alpha"), "acme"}};
  keys.Reload();  // A reads acme's key...
  redis.hashes[std::string(kMerchantKeysHash)] = {};  // ...which is then revoked,
  keys.Reload();  // and B, started after the revocation, reads none.
  redis.Release(1);  // B, its three reads one after the other
  redis.Release(1);
  redis.Release(1);
  EXPECT_EQ(keys.Resolve("rl_alpha"), nullptr);
  while (!redis.pending.empty()) redis.Release(0);  // A finishes last (a retry, say)
  EXPECT_EQ(keys.Resolve("rl_alpha"), nullptr);
}

TEST(Merchants, AMerchantWhoseLimitsDoNotChangeStaysTheSameAcrossReloads) {
  FakeHashes redis;
  MerchantKeys keys(redis);
  redis.Set({{Sha256Hex("rl_alpha"), "acme"}});
  redis.Set(std::string(kMerchantCapacityHash), {{"acme", "rate 100"}});
  ASSERT_TRUE(keys.ReloadAndWait(std::chrono::seconds(1)));
  const Merchant* before = keys.Resolve("rl_alpha");
  for (int i = 0; i < 100; ++i) ASSERT_TRUE(keys.ReloadAndWait(std::chrono::seconds(1)));
  EXPECT_EQ(keys.Resolve("rl_alpha"), before);  // a reload adds nothing that a check could hold
  redis.Set(std::string(kMerchantCapacityHash), {{"acme", "rate 200"}});
  ASSERT_TRUE(keys.ReloadAndWait(std::chrono::seconds(1)));
  const Merchant* after = keys.Resolve("rl_alpha");
  EXPECT_NE(after, before);
  EXPECT_EQ(after->limits.capacity->limit, 200u);
  EXPECT_EQ(before->limits.capacity->limit, 100u);  // still readable by a check that holds it
}

// Records the keys the engine is asked about, and the merchant each came with.
class RecordingCore final : public RateLimiterCore {
 public:
  void Check(const CheckRequest& request, CheckCallback done) override {
    {
      std::lock_guard lock(mu);
      keys.emplace_back(request.key);
      merchants.emplace_back(request.merchant != nullptr ? request.merchant->merchant : "-");
    }
    done(CheckResult{.allowed = true});
  }
  std::mutex mu;
  std::vector<std::string> keys, merchants;
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
  EXPECT_EQ(core_->merchants, (std::vector<std::string>{"acme", "globex", "acme", "acme"}));  // with its limits
}

}  // namespace
}  // namespace ratelimiter

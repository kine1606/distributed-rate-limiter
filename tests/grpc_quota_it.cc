// Week 2 gate: a Free customer is stopped at exactly 200 through gRPC. Week 4 gate: request 2,001
// gives three answers on three overage policies. The real service, CompositeCore over StrictCore,
// PlanStore and cluster backend run in-process against the dev cluster, with config/rules.yaml.
// Skipped, not failed, when the cluster is unreachable (seed: RL_TEST_CLUSTER, see it_cluster.h).
#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "config/rule_config.h"
#include "core/composite_core.h"
#include "core/strict_core.h"
#include "it_cluster.h"
#include "plan/plan_store.h"
#include "redis/keys.h"
#include "redis/redispp_backend.h"
#include "server/merchants.h"
#include "server/service.h"

namespace ratelimiter {
namespace {

class GrpcQuotaIT : public ::testing::Test {
 protected:
  void SetUp() override {
#if defined(__SANITIZE_THREAD__)
    // Ubuntu's gRPC is not TSan-instrumented: its arena reuse shows up as races in every call.
    GTEST_SKIP() << "needs a TSan-instrumented gRPC";
#endif
    admin_ = ConnectAdmin();
    if (!admin_) GTEST_SKIP() << "Redis Cluster not reachable at " << ClusterSeed().first << ":" << ClusterSeed().second;
    const std::string src = RL_SOURCE_DIR;
    RuleTable rules = LoadRules(src + "/config/rules.yaml");
    rules_ = rules;
    quota_rule_ = rules.at("daily_quota");
    burst_rule_ = rules.at("burst");
    backend_ = std::make_unique<RedisPPBackend>(ClusterSeed().first, ClusterSeed().second, src + "/lua");
    plans_ = std::make_unique<PlanStore>(*backend_);
    strict_ = std::make_unique<StrictCore>(*backend_, *plans_, rules);
    service_ = std::make_unique<RateLimiterService>(std::make_shared<CompositeCore>(
        std::make_shared<StrictCore>(*backend_, *plans_, rules), *strict_, *plans_, deny_, std::move(rules)));

    grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(service_.get());
    server_ = builder.BuildAndStart();
    ASSERT_TRUE(server_ && port > 0);
    stub_ = v1::RateLimiter::NewStub(grpc::CreateChannel("127.0.0.1:" + std::to_string(port),
                                                         grpc::InsecureChannelCredentials()));
  }

  void TearDown() override {
    if (server_) server_->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(1));
  }

  // Fresh customer on the given plan: plan hash written, today's usage and burst bucket gone.
  void Seed(const std::string& customer, const std::string& daily, const std::string& plan_name = "free",
            const std::string& overage = "hard", const std::string& extra = "0") {
    admin_->del(RedisKey(quota_rule_, customer));
    admin_->del(RedisKey(burst_rule_, customer));
    admin_->del(PlanKey(customer));
    const std::vector<std::pair<std::string, std::string>> plan{
        {"plan", plan_name}, {"daily", daily}, {"burst", "2"},       {"refill", "2"},
        {"overage", overage}, {"extra", extra}};
    admin_->hset(PlanKey(customer), plan.begin(), plan.end());
  }

  // As if `used` units were consumed today (the day number the script derives from Redis TIME).
  void SetUsedToday(const std::string& customer, int used) {
    const std::string key = RedisKey(quota_rule_, customer);
    const auto t = admin_->redis(key, false).command<std::vector<std::string>>("TIME");  // the script's clock
    const std::string day = std::to_string(std::stoll(t.at(0)) / 86400);
    const std::vector<std::pair<std::string, std::string>> state{{"day", day}, {"used", std::to_string(used)}};
    admin_->hset(key, state.begin(), state.end());
  }

  grpc::Status Check(const std::string& customer, v1::CheckResponse* resp, const std::string& rule = "daily_quota",
                     const std::string& request_id = "") {
    v1::CheckRequest req;
    req.set_key(customer);
    req.set_rule(rule);
    req.set_request_id(request_id);
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
    return stub_->Check(&ctx, req, resp);
  }

  std::unique_ptr<sw::redis::RedisCluster> admin_;
  Rule quota_rule_, burst_rule_;
  RuleTable rules_;
  std::unique_ptr<RedisPPBackend> backend_;
  std::unique_ptr<PlanStore> plans_;
  DenyCache deny_;
  std::unique_ptr<StrictCore> strict_;
  std::unique_ptr<RateLimiterService> service_;
  std::unique_ptr<grpc::Server> server_;  // after the service: shut down and destroyed first
  std::unique_ptr<v1::RateLimiter::Stub> stub_;
};

// Phase 2, P1, end to end: a merchant's API key registered in Redis as rlctl does (hashed), loaded by
// the real backend; a call without it or with another key is refused; with it, the merchant's customer
// is limited under "<merchant>|<customer>", and the same id without tenancy is someone else.
TEST_F(GrpcQuotaIT, AMerchantsKeyNamesItsOwnCustomers) {
  const std::string api_key = "rl_it_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  admin_->hset(std::string(kMerchantKeysHash), Sha256Hex(api_key), "itmerchant");
  Seed("itmerchant|cust:t1", "2");
  Seed("cust:t1", "5");
  MerchantKeys merchants(*backend_);
  ASSERT_TRUE(merchants.ReloadAndWait(std::chrono::seconds(2)));
  RateLimiterService tenant_service(std::make_shared<CompositeCore>(std::make_shared<StrictCore>(*backend_, *plans_, rules_),
                                                                    *strict_, *plans_, deny_, rules_),
                                    10'000, nullptr, &merchants);
  grpc::ServerBuilder builder;
  int port = 0;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
  builder.RegisterService(&tenant_service);
  auto server = builder.BuildAndStart();
  ASSERT_TRUE(server && port > 0);
  auto stub = v1::RateLimiter::NewStub(grpc::CreateChannel("127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials()));
  const auto check = [&stub](const std::string& key, v1::CheckResponse* resp) {
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
    if (!key.empty()) ctx.AddMetadata(std::string(kApiKeyHeader), key);
    v1::CheckRequest req;
    req.set_key("cust:t1");
    req.set_rule("customer");
    return stub->Check(&ctx, req, resp);
  };
  v1::CheckResponse resp;
  EXPECT_EQ(check("", &resp).error_code(), grpc::StatusCode::UNAUTHENTICATED);
  EXPECT_EQ(check("rl_someone_else", &resp).error_code(), grpc::StatusCode::UNAUTHENTICATED);
  for (int i = 0; i < 2; ++i) {
    ASSERT_TRUE(check(api_key, &resp).ok());
    EXPECT_TRUE(resp.allowed());
    EXPECT_EQ(resp.limit(), 2u);  // the merchant's customer's plan, not cust:t1's
  }
  ASSERT_TRUE(check(api_key, &resp).ok());
  EXPECT_FALSE(resp.allowed());
  EXPECT_EQ(admin_->hget(RedisKey(quota_rule_, "itmerchant|cust:t1"), "used").value_or(""), "2");
  EXPECT_FALSE(admin_->exists(RedisKey(quota_rule_, "cust:t1")));  // the untenanted cust:t1 was never charged
  server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(1));
  admin_->hdel(std::string(kMerchantKeysHash), Sha256Hex(api_key));
}

// 400 concurrent requests from 8 threads for one Free customer: exactly 200 pass, the other 200 are
// told the quota is exhausted until the daily reset, and Redis counted 200.
TEST_F(GrpcQuotaIT, FreeCustomerIsStoppedAtExactly200) {
  const std::string customer = "cust:it-gate";
  Seed(customer, "200");

  std::atomic<int> allowed{0}, exhausted{0}, other{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&] {
      for (int i = 0; i < 50; ++i) {
        v1::CheckResponse resp;
        const grpc::Status s = Check(customer, &resp);
        if (!s.ok()) {
          ++other;
        } else if (resp.allowed() && resp.reason() == v1::REASON_OK && resp.limit() == 200) {
          ++allowed;
        } else if (!resp.allowed() && resp.reason() == v1::REASON_QUOTA_EXHAUSTED && resp.reset_after_ms() > 0 &&
                   resp.retry_after_ms() == resp.reset_after_ms() && resp.remaining() == 0) {
          ++exhausted;
        } else {
          ++other;
        }
      }
    });
  }
  for (auto& t : threads) t.join();

  EXPECT_EQ(allowed.load(), 200);
  EXPECT_EQ(exhausted.load(), 200);
  EXPECT_EQ(other.load(), 0);
  EXPECT_EQ(admin_->hget(RedisKey(quota_rule_, customer), "used").value_or("missing"), "200");
}

// The limit comes from the plan, read on every check: another plan, another limit (step B2).
TEST_F(GrpcQuotaIT, RemainingAndLimitFollowTheCustomersPlan) {
  const std::string customer = "cust:it-pay";
  Seed(customer, "2000");
  v1::CheckResponse resp;
  ASSERT_TRUE(Check(customer, &resp).ok());
  EXPECT_TRUE(resp.allowed());
  EXPECT_EQ(resp.limit(), 2000u);
  EXPECT_EQ(resp.remaining(), 1999u);
  EXPECT_GT(resp.reset_after_ms(), 0u);
}

// Week 4 gate (step B4): the same request 2,001 through the customer rule on a hard, a grace and a
// cap plan of 2,000 a day: denied, allowed free, allowed as billed overage.
TEST_F(GrpcQuotaIT, Request2001GivesThreeAnswersOnThreePolicies) {
  Seed("cust:it-hard", "2000", "free", "hard", "0");
  Seed("cust:it-grace", "2000", "pay", "grace", "200");
  Seed("cust:it-cap", "2000", "pay_cap", "cap", "1000");
  for (const char* c : {"cust:it-hard", "cust:it-grace", "cust:it-cap"}) SetUsedToday(c, 2000);

  v1::CheckResponse hard, grace, cap;
  ASSERT_TRUE(Check("cust:it-hard", &hard, "customer").ok());
  ASSERT_TRUE(Check("cust:it-grace", &grace, "customer").ok());
  ASSERT_TRUE(Check("cust:it-cap", &cap, "customer").ok());
  EXPECT_FALSE(hard.allowed());
  EXPECT_EQ(hard.reason(), v1::REASON_QUOTA_EXHAUSTED);
  EXPECT_EQ(hard.retry_after_ms(), hard.reset_after_ms());
  EXPECT_TRUE(grace.allowed());
  EXPECT_EQ(grace.reason(), v1::REASON_OK);
  EXPECT_EQ(grace.remaining(), 0u);
  EXPECT_TRUE(cap.allowed());
  EXPECT_EQ(cap.reason(), v1::REASON_OVERAGE);
  EXPECT_EQ(admin_->hget(RedisKey(quota_rule_, "cust:it-cap"), "overage_used").value_or(""), "1");
  // The hard customer's burst token went back: the bucket (capacity 2) is full again.
  EXPECT_EQ(admin_->hget(RedisKey(burst_rule_, "cust:it-hard"), "tokens").value_or(""), "2000");
}

TEST_F(GrpcQuotaIT, SuspendedCustomerIsDeniedAndRetriedRequestsCountOnce) {
  Seed("cust:it-susp", "0", "suspended");
  v1::CheckResponse s;
  ASSERT_TRUE(Check("cust:it-susp", &s, "customer").ok());
  EXPECT_FALSE(s.allowed());
  EXPECT_EQ(s.reason(), v1::REASON_SUSPENDED);

  Seed("cust:it-retry", "200");
  admin_->del(RequestKey(quota_rule_, "cust:it-retry", "req-1"));  // an earlier run's answer lives 5 min
  v1::CheckResponse first, retry;
  ASSERT_TRUE(Check("cust:it-retry", &first, "customer", "req-1").ok());
  ASSERT_TRUE(Check("cust:it-retry", &retry, "customer", "req-1").ok());
  EXPECT_TRUE(retry.allowed());
  EXPECT_EQ(retry.remaining(), first.remaining());
  EXPECT_EQ(admin_->hget(RedisKey(quota_rule_, "cust:it-retry"), "used").value_or(""), "1");
}

}  // namespace
}  // namespace ratelimiter

// Week 2 gate: a Free customer is stopped at exactly 200 through gRPC. The real service, StrictCore,
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
#include "core/strict_core.h"
#include "it_cluster.h"
#include "plan/plan_store.h"
#include "redis/keys.h"
#include "redis/redispp_backend.h"
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
    quota_rule_ = rules.at("daily_quota");
    backend_ = std::make_unique<RedisPPBackend>(ClusterSeed().first, ClusterSeed().second, src + "/lua");
    plans_ = std::make_unique<PlanStore>(*backend_);
    service_ = std::make_unique<RateLimiterService>(std::make_shared<StrictCore>(*backend_, *plans_, std::move(rules)));

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

  // Fresh customer on the given plan: plan hash written, today's usage gone.
  void Seed(const std::string& customer, const std::string& daily) {
    admin_->del(RedisKey(quota_rule_, customer));
    admin_->del(PlanKey(customer));
    const std::vector<std::pair<std::string, std::string>> plan{
        {"plan", "free"}, {"daily", daily}, {"burst", "2"}, {"refill", "2"}};
    admin_->hset(PlanKey(customer), plan.begin(), plan.end());
  }

  grpc::Status Check(const std::string& customer, v1::CheckResponse* resp) {
    v1::CheckRequest req;
    req.set_key(customer);
    req.set_rule("daily_quota");
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
    return stub_->Check(&ctx, req, resp);
  }

  std::unique_ptr<sw::redis::RedisCluster> admin_;
  Rule quota_rule_;
  std::unique_ptr<RedisPPBackend> backend_;
  std::unique_ptr<PlanStore> plans_;
  std::unique_ptr<RateLimiterService> service_;
  std::unique_ptr<grpc::Server> server_;  // after the service: shut down and destroyed first
  std::unique_ptr<v1::RateLimiter::Stub> stub_;
};

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

}  // namespace
}  // namespace ratelimiter

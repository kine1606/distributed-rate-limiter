// The gRPC service (src/server/service.h) on an in-process channel, no sockets: request validation
// at the trust boundary, status versus reason, and CheckBatch's ordering and limits. A fake engine
// answers inline, or later from other threads in reverse order.
#include "server/service.h"

#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ratelimiter {
namespace {

class FakeCore final : public RateLimiterCore {
 public:
  void Check(const CheckRequest& request, CheckCallback done) override {
    const CheckResult r = answer(request);
    std::unique_lock lock(mu);
    seen.push_back(std::string(request.key) + "/" + std::string(request.rule) + "/" + std::to_string(request.cost));
    if (!late) {
      lock.unlock();
      done(r);
      return;
    }
    // Later calls answer first: each waits less than the one before it.
    const auto wait = std::chrono::milliseconds(50) - std::chrono::microseconds(40 * seen.size());
    threads.emplace_back([r, wait, done = std::move(done)] {
      std::this_thread::sleep_for(wait);
      done(r);
    });
  }
  ~FakeCore() override {
    for (auto& t : threads) t.join();
  }

  std::function<CheckResult(const CheckRequest&)> answer = [](const CheckRequest& q) {
    return CheckResult{.allowed = true, .remaining = q.cost};  // echoes the cost: shows whose answer it is
  };
  bool late = false;
  std::mutex mu;
  std::vector<std::string> seen;
  std::vector<std::thread> threads;
};

class ServiceTest : public ::testing::Test {
 protected:
  void SetUp() override {
#if defined(__SANITIZE_THREAD__)
    GTEST_SKIP() << "needs a TSan-instrumented gRPC";  // as in grpc_quota_it.cc
#endif
    grpc::ServerBuilder builder;
    builder.RegisterService(&service_);
    server_ = builder.BuildAndStart();
    ASSERT_TRUE(server_);
    stub_ = v1::RateLimiter::NewStub(server_->InProcessChannel(grpc::ChannelArguments()));
  }
  void TearDown() override {
    if (server_) server_->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(1));
  }

  friend class AdmissionTest;
  friend class ServiceMetrics_EveryAnswerIsCountedByRuleAndReason_Test;
  static v1::CheckRequest Req(const std::string& key, const std::string& rule = "api_default", std::uint32_t cost = 0) {
    v1::CheckRequest r;
    r.set_key(key);
    r.set_rule(rule);
    r.set_cost(cost);
    return r;
  }
  grpc::Status Check(const v1::CheckRequest& req, v1::CheckResponse* resp) {
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
    return stub_->Check(&ctx, req, resp);
  }
  grpc::Status Batch(const std::vector<v1::CheckRequest>& reqs, v1::CheckBatchResponse* resp) {
    v1::CheckBatchRequest batch;
    for (const auto& r : reqs) *batch.add_requests() = r;
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
    return stub_->CheckBatch(&ctx, batch, resp);
  }

  std::shared_ptr<FakeCore> core_ = std::make_shared<FakeCore>();
  RateLimiterService service_{core_};
  std::unique_ptr<grpc::Server> server_;  // after the service: shut down and destroyed first
  std::unique_ptr<v1::RateLimiter::Stub> stub_;
};

TEST_F(ServiceTest, AllowedCheckFillsTheResponse) {
  v1::CheckResponse resp;
  ASSERT_TRUE(Check(Req("cust:1", "api_default", 3), &resp).ok());
  EXPECT_TRUE(resp.allowed());
  EXPECT_EQ(resp.reason(), v1::REASON_OK);
  EXPECT_EQ(resp.remaining(), 3u);
  EXPECT_EQ(core_->seen, std::vector<std::string>{"cust:1/api_default/3"});
}

TEST_F(ServiceTest, MalformedRequestsNeverReachTheEngine) {
  v1::CheckResponse resp;
  EXPECT_EQ(Check(Req(""), &resp).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(Check(Req("k", ""), &resp).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(Check(Req(std::string(129, 'k')), &resp).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  auto r = Req("k");
  r.set_request_id(std::string(129, 'i'));
  EXPECT_EQ(Check(r, &resp).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_TRUE(core_->seen.empty());
}

TEST_F(ServiceTest, UnknownRuleIsInvalidArgument) {
  core_->answer = [](const CheckRequest&) { return CheckResult{.reason = Reason::kUnknownRule}; };
  v1::CheckResponse resp;
  const auto s = Check(Req("k", "no_such_rule"), &resp);
  EXPECT_EQ(s.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(s.error_message(), "unknown rule");
}

// A Redis outage is an answer, not an RPC failure: the gateway reads the reason (proto, enum Reason).
TEST_F(ServiceTest, RedisOutageIsAReasonInAnOkResponse) {
  core_->answer = [](const CheckRequest&) { return CheckResult{.reason = Reason::kUnavailable}; };
  v1::CheckResponse resp;
  ASSERT_TRUE(Check(Req("k"), &resp).ok());
  EXPECT_FALSE(resp.allowed());
  EXPECT_EQ(resp.reason(), v1::REASON_UNAVAILABLE);
}

TEST_F(ServiceTest, AnAnswerFromAnotherThreadCompletesTheCall) {  // the Redis I/O thread in production
  core_->late = true;
  v1::CheckResponse resp;
  ASSERT_TRUE(Check(Req("k", "r", 7), &resp).ok());
  EXPECT_EQ(resp.remaining(), 7u);
}

TEST_F(ServiceTest, EmptyBatchIsOkAndEmpty) {
  v1::CheckBatchResponse resp;
  ASSERT_TRUE(Batch({}, &resp).ok());
  EXPECT_EQ(resp.responses_size(), 0);
}

// Answers arrive in reverse order on different threads; each lands in its request's slot.
TEST_F(ServiceTest, BatchAnswersKeepTheRequestOrder) {
  core_->late = true;
  std::vector<v1::CheckRequest> reqs;
  for (std::uint32_t i = 1; i <= 50; ++i) reqs.push_back(Req("k" + std::to_string(i), "r", i));
  v1::CheckBatchResponse resp;
  ASSERT_TRUE(Batch(reqs, &resp).ok());
  ASSERT_EQ(resp.responses_size(), 50);
  for (int i = 0; i < 50; ++i) EXPECT_EQ(resp.responses(i).remaining(), static_cast<std::uint32_t>(i + 1)) << i;
}

TEST_F(ServiceTest, BatchOf1000IsServedAnd1001IsRejected) {
  v1::CheckBatchResponse resp;
  ASSERT_TRUE(Batch(std::vector<v1::CheckRequest>(1000, Req("k")), &resp).ok());
  EXPECT_EQ(resp.responses_size(), 1000);
  core_->seen.clear();
  v1::CheckBatchResponse rejected;
  EXPECT_EQ(Batch(std::vector<v1::CheckRequest>(1001, Req("k")), &rejected).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_TRUE(core_->seen.empty());
}

TEST_F(ServiceTest, OneMalformedEntryRejectsTheBatchBeforeAnyCheck) {  // nothing is counted twice on a resend
  v1::CheckBatchResponse resp;
  EXPECT_EQ(Batch({Req("a"), Req(""), Req("c")}, &resp).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_TRUE(core_->seen.empty());
}

TEST_F(ServiceTest, OneUnknownRuleFailsTheWholeBatch) {
  core_->late = true;
  core_->answer = [](const CheckRequest& q) {
    return q.rule == "bad" ? CheckResult{.reason = Reason::kUnknownRule} : CheckResult{.allowed = true};
  };
  v1::CheckBatchResponse resp;
  EXPECT_EQ(Batch({Req("a"), Req("b", "bad"), Req("c")}, &resp).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(core_->seen.size(), 3u);  // the others were checked; the RPC still fails as a whole
}

// A rule reload (SIGHUP) swaps the engine under load: a check already in the old engine is answered
// by it, and every check after the swap goes to the new one.
TEST_F(ServiceTest, AfterSetCoreNewChecksGoToTheNewCoreAndOldOnesFinish) {
  core_->late = true;
  auto in_old = std::async(std::launch::async, [this] {
    v1::CheckResponse resp;
    const grpc::Status s = Check(Req("old", "r", 1), &resp);
    return std::make_pair(s.ok(), resp.remaining());
  });
  for (bool seen = false; !seen; std::this_thread::sleep_for(std::chrono::milliseconds(1))) {
    std::lock_guard lock(core_->mu);
    seen = !core_->seen.empty();
  }
  auto next = std::make_shared<FakeCore>();
  next->answer = [](const CheckRequest&) { return CheckResult{.allowed = true, .remaining = 99}; };
  service_.SetCore(next);
  v1::CheckResponse resp;
  ASSERT_TRUE(Check(Req("new"), &resp).ok());
  EXPECT_EQ(resp.remaining(), 99u);
  EXPECT_EQ(in_old.get(), std::make_pair(true, 1u));  // answered late, by the old engine
  EXPECT_EQ(next->seen, std::vector<std::string>{"new/api_default/1"});  // a cost of 0 means 1
}

// ---- metrics (Week 5) ----

TEST(ServiceMetrics, EveryAnswerIsCountedByRuleAndReason) {
#if defined(__SANITIZE_THREAD__)
  GTEST_SKIP() << "needs a TSan-instrumented gRPC";
#endif
  const RuleTable rules{{"api_default", Rule{.name = "api_default"}}};
  CheckMetrics metrics(rules);
  auto core = std::make_shared<FakeCore>();
  core->answer = [](const CheckRequest& q) {
    return q.rule == "api_default" ? CheckResult{.allowed = true} : CheckResult{.reason = Reason::kUnknownRule};
  };
  RateLimiterService service(core, 10'000, &metrics);
  grpc::ServerBuilder builder;
  builder.RegisterService(&service);
  auto server = builder.BuildAndStart();
  auto stub = v1::RateLimiter::NewStub(server->InProcessChannel(grpc::ChannelArguments()));
  const auto call = [&](const std::string& key, const std::string& rule) {
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
    v1::CheckResponse resp;
    return stub->Check(&ctx, ServiceTest::Req(key, rule), &resp);
  };
  EXPECT_TRUE(call("k", "api_default").ok());
  EXPECT_TRUE(call("k", "api_default").ok());
  EXPECT_FALSE(call("", "api_default").ok());  // malformed
  EXPECT_FALSE(call("k", "no_such_rule").ok());  // unknown: no series of its own
  std::string out;
  metrics.Render(out);
  EXPECT_NE(out.find("rl_checks_total{rule=\"api_default\",reason=\"ok\"} 2\n"), std::string::npos) << out;
  EXPECT_NE(out.find("rl_check_latency_seconds_count{rule=\"api_default\"} 2\n"), std::string::npos);
  EXPECT_NE(out.find("rl_rejected_total{reason=\"invalid\"} 2\n"), std::string::npos);
  EXPECT_EQ(out.find("no_such_rule"), std::string::npos);
  server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(1));
}

// ---- backpressure (Week 5) ----

// Holds every answer until Release(), so a test can fill the service up.
class HoldingCore final : public RateLimiterCore {
 public:
  void Check(const CheckRequest&, CheckCallback done) override {
    std::lock_guard lock(mu);
    held.push_back(std::move(done));
  }
  void Release() {
    std::vector<CheckCallback> out;
    {
      std::lock_guard lock(mu);
      out.swap(held);
    }
    for (auto& d : out) d(CheckResult{.allowed = true});
  }
  std::size_t Held() {
    std::lock_guard lock(mu);
    return held.size();
  }
  std::mutex mu;
  std::vector<CheckCallback> held;
};

class AdmissionTest : public ::testing::Test {
 protected:
  void SetUp() override {
#if defined(__SANITIZE_THREAD__)
    GTEST_SKIP() << "needs a TSan-instrumented gRPC";
#endif
    grpc::ServerBuilder builder;
    builder.RegisterService(&service_);
    server_ = builder.BuildAndStart();
    ASSERT_TRUE(server_);
    stub_ = v1::RateLimiter::NewStub(server_->InProcessChannel(grpc::ChannelArguments()));
  }
  void TearDown() override {
    core_->Release();
    if (server_) server_->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(1));
  }
  grpc::Status Call(int batch = 0) {
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
    if (batch == 0) {
      v1::CheckResponse resp;
      return stub_->Check(&ctx, ServiceTest::Req("k"), &resp);
    }
    v1::CheckBatchRequest req;
    for (int i = 0; i < batch; ++i) *req.add_requests() = ServiceTest::Req("k");
    v1::CheckBatchResponse resp;
    return stub_->CheckBatch(&ctx, req, &resp);
  }
  // Starts a call that the core holds; its status lands in the returned future.
  std::future<grpc::Status> Hold() {
    auto p = std::make_shared<std::promise<grpc::Status>>();
    auto ctx = std::make_shared<grpc::ClientContext>();
    auto resp = std::make_shared<v1::CheckResponse>();
    auto req = std::make_shared<v1::CheckRequest>(ServiceTest::Req("k"));
    stub_->async()->Check(ctx.get(), req.get(), resp.get(), [p, ctx, resp, req](grpc::Status s) { p->set_value(s); });
    return p->get_future();
  }
  bool WaitHeld(std::size_t n) {
    for (int i = 0; i < 500 && core_->Held() < n; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    return core_->Held() == n;
  }

  std::shared_ptr<HoldingCore> core_ = std::make_shared<HoldingCore>();
  RateLimiterService service_{core_, 2};
  std::unique_ptr<grpc::Server> server_;
  std::unique_ptr<v1::RateLimiter::Stub> stub_;
};

TEST_F(AdmissionTest, BeyondTheLimitACallIsRefusedAtOnce) {
  auto a = Hold();
  auto b = Hold();
  ASSERT_TRUE(WaitHeld(2));
  EXPECT_EQ(service_.InFlight(), 2);
  const auto refused = Call();
  EXPECT_EQ(refused.error_code(), grpc::StatusCode::RESOURCE_EXHAUSTED);  // not queued behind the two
  EXPECT_EQ(core_->Held(), 2u);
  core_->Release();
  EXPECT_TRUE(a.get().ok());
  EXPECT_TRUE(b.get().ok());
  EXPECT_EQ(service_.InFlight(), 0);
  auto c = Hold();  // room again
  ASSERT_TRUE(WaitHeld(1));
  core_->Release();
  EXPECT_TRUE(c.get().ok());
}

TEST_F(AdmissionTest, ABatchCountsEveryEntry) {
  // 3 entries can never fit a limit of 2: invalid, not an overload to retry (code review, Week 5)
  EXPECT_EQ(Call(3).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  std::thread release([this] {
    WaitHeld(2);
    core_->Release();
  });
  EXPECT_TRUE(Call(2).ok());
  release.join();
  EXPECT_EQ(service_.InFlight(), 0);  // the batch left all at once
}

}  // namespace
}  // namespace ratelimiter

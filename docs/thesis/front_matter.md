::: {custom-style="Title page"}
UNIVERSITY OF INFORMATION TECHNOLOGY, VNU-HCM

FACULTY OF SOFTWARE ENGINEERING

GRADUATION THESIS

**MULTI-TENANT QUOTA AND RATE LIMITING SERVICE**

Student: Nguyen Trung Kien · Student ID: **[to fill]**

Advisor: **[to fill]**

Ho Chi Minh City, December 2026
:::

```{=openxml}
<w:p><w:r><w:br w:type="page"/></w:r></w:p>
```

# Acknowledgments {.unnumbered}

**[To write in your own words.]** A suggested outline: thank your advisor, **[name]**, for the guidance
on the design and the evaluation; the Faculty of Software Engineering for the training behind this
work; and your family and friends for their support.

# Abstract {.unnumbered}

Companies that sell access to their APIs sell quotas by plan, and every call must be checked
against its customer's plan in real time. The check must be exact, because quotas are billed; fast,
because it sits in front of every call; and shared, because many servers answer for the same
customer. Existing gateways treat quotas as best-effort targets, or give up exactness for speed.

This thesis builds a quota and rate limiting service in C++20, gRPC and Redis Cluster, and splits the
problem in two.
- **Daily quotas** are decided in Redis by atomic Lua scripts. They are exact, deduplicated by request
  id, reset once a day by Redis's clock, and correct under plan changes and three overage policies.
- **Burst limits** are served from leases: tokens an instance takes from Redis in chunks and hands
  out with one atomic operation. Over any window they admit at most N × C beyond an exact limiter, for
  N instances and a chunk cap C; the bound is proved and measured.
- **Around both:** a deny cache answers customers out of quota without Redis, a plan cache is kept
  fresh by Pub/Sub, and each plan has a policy for when Redis fails.

The evaluation tests the two halves of the claim separately, on one laptop.
- **Exactness:** quotas were exact in 20 of 20 adversarial runs on four instances, and 22.6 million
  checks matched a reference model.
- **The bound:** over-admission measured 34.7, 189.0 and 381.4 tokens against bounds of 40, 200 and
  400, within 0.3 tokens of a model.
- **Speed:** a leased check costs about as much as gRPC alone: up to 105,000 checks per second per
  instance, 400,000 with batches of 10, and a p99 of 0.58 ms at 10,000 per second. A full quota check
  makes two Redis round trips: 26,000 to 42,000 per second per instance.
- **Failures:** a slow, silent or killed Redis master affected only its own customers, and no call
  failed.

From these measurements, 1,000,000 checks per second is estimated to need 15 to 50 instances and 17
Redis masters; it was not measured.

**Keywords:** rate limiting, API quotas, multi-tenancy, Redis Cluster, leases, gRPC, C++.

# Tóm tắt {.unnumbered}

Các công ty bán quyền truy cập API bán hạn mức (quota) theo gói, và mỗi lời gọi phải được kiểm tra với
gói của khách hàng ngay lập tức. Việc kiểm tra phải chính xác vì hạn mức được tính tiền, phải nhanh vì
nó đứng trước mọi lời gọi, và phải dùng chung vì nhiều máy chủ cùng trả lời cho một khách hàng. Các
cổng API hiện có coi hạn mức là mục tiêu gần đúng, hoặc đánh đổi độ chính xác lấy tốc độ.

Khóa luận xây dựng một dịch vụ giới hạn hạn mức và tốc độ bằng C++20, gRPC và Redis Cluster, chia bài
toán làm hai phần.
- **Hạn mức theo ngày** được quyết định trong Redis bằng các script Lua nguyên tử. Hạn mức chính xác,
  không đếm trùng nhờ mã yêu cầu, được đặt lại mỗi ngày một lần theo đồng hồ của Redis, và đúng khi
  khách hàng đổi gói cũng như với ba chính sách vượt hạn mức.
- **Giới hạn tức thời (burst)** được phục vụ từ các "lease": mỗi máy chủ lấy trước từ Redis một phần
  token và cấp phát cục bộ bằng một thao tác nguyên tử. Trong mọi khoảng thời gian, số lời gọi được
  cho qua vượt quá một bộ giới hạn chính xác nhiều nhất là N × C, với N máy chủ và kích thước phần C;
  cận này được chứng minh và đo đạc.
- **Xung quanh hai phần trên:** bộ nhớ đệm từ chối trả lời khách hàng đã hết hạn mức mà không cần hỏi
  Redis, bộ nhớ đệm gói được cập nhật qua Pub/Sub, và mỗi gói có chính sách riêng khi Redis gặp sự cố.

Phần đánh giá kiểm tra riêng từng nửa của luận điểm, trên một máy tính xách tay.
- **Độ chính xác:** hạn mức chính xác trong 20/20 lần chạy với bốn máy chủ, và 22,6 triệu lượt kiểm
  tra khớp với một mô hình tham chiếu.
- **Cận vượt mức:** đo được 34,7, 189,0 và 381,4 token so với cận 40, 200 và 400, lệch mô hình dưới
  0,3 token.
- **Tốc độ:** một lượt kiểm tra qua lease tốn gần như bằng gRPC không có bộ giới hạn: đến 105.000
  lượt/giây mỗi máy chủ, 400.000 khi gộp 10 lượt mỗi lời gọi, p99 0,58 ms ở 10.000 lượt/giây. Một
  lượt kiểm tra hạn mức đầy đủ cần hai lần hỏi Redis: 26.000 đến 42.000 lượt/giây mỗi máy chủ.
- **Sự cố:** một nút Redis chậm, im lặng hoặc bị tắt chỉ ảnh hưởng đến khách hàng của chính nút đó,
  và không lời gọi nào thất bại.

Từ các số đo này, mức 1.000.000 lượt/giây được ước tính cần 15 đến 50 máy chủ và 17 nút Redis chính;
con số này là ước tính, chưa được đo.

**Từ khóa:** giới hạn tốc độ, hạn mức API, đa khách hàng, Redis Cluster, lease, gRPC, C++.

# Abbreviations {.unnumbered}

| Abbreviation | Meaning |
| --- | --- |
| API | Application Programming Interface |
| ASan, TSan | AddressSanitizer, ThreadSanitizer |
| CAS | compare-and-swap, an atomic CPU instruction |
| gRPC | an RPC framework over HTTP/2, from Google |
| HTTP | Hypertext Transfer Protocol |
| LTO, PGO | link-time optimisation, profile-guided optimisation |
| N × C | instances times chunk cap: the bound on a lease's over-admission |
| p50, p99, p999 | the 50th, 99th and 99.9th percentiles of latency |
| RPC | remote procedure call |
| TTL | time to live (of a cache entry or a Redis key) |
| USL | Universal Scalability Law |
| UTC | Coordinated Universal Time |

```{=openxml}
<w:p><w:r><w:br w:type="page"/></w:r></w:p>
```

::: {custom-style="TOC Heading"}
Contents
:::

```{=openxml}
<w:sdt><w:sdtPr><w:docPartObj><w:docPartGallery w:val="Table of Contents"/><w:docPartUnique/></w:docPartObj></w:sdtPr><w:sdtContent><w:p><w:r><w:fldChar w:fldCharType="begin" w:dirty="true"/><w:instrText xml:space="preserve">TOC \o "1-2" \h \z \u</w:instrText><w:fldChar w:fldCharType="separate"/><w:t>Right-click and choose Update Field to build the contents.</w:t><w:fldChar w:fldCharType="end"/></w:r></w:p></w:sdtContent></w:sdt>
```

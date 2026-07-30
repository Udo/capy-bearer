#pragma once

#include "types.h"

// Internal durable task transport. It intentionally carries only copied values.
namespace task_queue {

struct Limits {
	u64 capacity = 0;
	u64 payload_max = 0;
	u64 retention_seconds = 0;
	u64 scan_max = 0;
	u64 status_max = 0;
	u64 await_max_ms = 0;
};

struct Task {
	String id;
	String target;
	DValue props;
	String lease_id;
	u64 attempt = 0;
};

struct Result {
	String code;
	Task task;
	DValue status;
	u64 count = 0;
	bool ok() const { return(code == "ok"); }
};

Result submit(const String& root, const Limits& limits, const String& target, const DValue& props);
Result claim(const String& root, const Limits& limits, const String& worker_lease_id);
Result succeed(const String& root, const Limits& limits, const String& id, const String& worker_lease_id);
Result fail(const String& root, const Limits& limits, const String& id, const String& worker_lease_id, const String& failure_code);
Result status(const String& root, const Limits& limits, const String& id);
Result await(const String& root, const Limits& limits, const String& id, u64 timeout_ms, u64 poll_ms);
Result cancel(const String& root, const Limits& limits, const String& id);
// Returns at most maximum id -> lease_id cancellation requests; never exposes worker PIDs.
Result cancellation_requests(const String& root, const Limits& limits, u64 maximum);
// Empty lease recovers every orphaned running record during parent startup.
Result recover_worker(const String& root, const Limits& limits, const String& worker_lease_id);
Result reap(const String& root, const Limits& limits);

#ifdef TASK_QUEUE_TESTING
void set_claim_crash_after_rename_for_test(bool enabled);
#endif

} // namespace task_queue

#pragma once

#include "task_queue.h"

// Native-only TASK admission/status seam.  The FastCGI parent owns its pool;
// request workers never call claim().  A target is canonicalized as
// /absolute/unit.{uce,capy}#TASK[:NAME] internally before it enters the durable queue.
namespace task_workers {

task_queue::Result submit(const String& target, const DValue& props);
task_queue::Result status(const String& id);
task_queue::Result await(const String& id, u64 timeout_ms);
task_queue::Result cancel(const String& id);

} // namespace task_workers

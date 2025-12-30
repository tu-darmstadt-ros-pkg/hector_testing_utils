# hector_testing_utils

Helper classes and functions for writing Google Tests that use the normal ROS 2 graph.

These utilities are intentionally heavier than rtest. Compared to rtest, they are slower and more brittle.
Prefer rtest for fast, deterministic unit tests.
Use this package only when you must spin real nodes (for example: libraries that auto-create
publishers/services, or tests that validate node-to-node interactions). Those tests are slower
and more brittle, so keep them focused and timeboxed.

## What is included

- CachedSubscriber: cache the last message, wait for messages (optionally with a predicate), and check connections.
- TestExecutor: spin nodes while waiting on conditions or futures.
- wait_for_* helpers: wait for publishers, subscribers, services, and action servers.
- call_service / call_action helpers with timeouts.
- node_options_from_yaml to load parameters from YAML into NodeOptions.

## Example

```cpp
#include <gtest/gtest.h>
#include <std_msgs/msg/int32.hpp>

#include <hector_testing_utils/hector_testing_utils.hpp>

using namespace std::chrono_literals;

TEST(Example, CachedSubscriber)
{
  auto node = std::make_shared<rclcpp::Node>("example_node");
  hector_testing_utils::TestExecutor executor;
  executor.add_node(node);

  const std::string topic = "/example/int32";
  auto pub = node->create_publisher<std_msgs::msg::Int32>(topic, 10);
  hector_testing_utils::CachedSubscriber<std_msgs::msg::Int32> sub(node, topic);

  ASSERT_TRUE(sub.wait_for_publishers(executor, 1, 5s));

  std_msgs::msg::Int32 msg;
  msg.data = 42;
  pub->publish(msg);

  ASSERT_TRUE(sub.wait_for_message(executor, 5s));
  auto last = sub.last_message();
  ASSERT_TRUE(last.has_value());
  EXPECT_EQ(last->data, 42);
}
```

## Timeouts and robustness

The helpers default to conservative timeouts (see kDefaultTimeout and the call option structs).
For slow CI pipelines, use longer timeouts instead of tight sleeps, and prefer the wait_for_*
helpers to avoid race conditions.


[![ROS2](https://img.shields.io/badge/ROS2-Jazzy%20|%20Kilted%20|%20Rolling-blue)](https://docs.ros.org)
![Lint](https://github.com/Joschi3/hector_testing_utils/actions/workflows/lint_build_test.yaml/badge.svg)
[![codecov](https://codecov.io/gh/Joschi3/hector_testing_utils/graph/badge.svg?token=RYR8J8FNC8)](https://codecov.io/gh/Joschi3/hector_testing_utils)

# hector_testing_utils

Helper classes and functions for writing Google Tests that use the normal ROS 2 graph.


## What is included

### Core Components

* **TestContext**: Manages a scoped ROS 2 context for test isolation
* **TestExecutor**: Wraps a single-threaded executor with convenient helpers for spinning until conditions are met
* **TestNode**: Enhanced node class with factory methods and connection tracking
* **HectorTestFixture / HectorTestFixtureWithContext**: Google Test fixtures with pre-configured executor and test node

### Connection-Aware Wrappers

* **TestPublisher**: Publisher wrapper that tracks subscriber connections
* **TestSubscription**: Subscription wrapper that caches messages and tracks publisher connections
* **TestClient**: Service client wrapper with connection awareness
* **TestServiceServer**: Service server wrapper that tracks client connections
* **TestActionClient**: Action client wrapper with server readiness checking
* **TestActionServer**: Action server wrapper that tracks client connections


### Wait Helpers

* `wait_for_publishers`: Wait for publishers to appear on a topic
* `wait_for_subscribers`: Wait for subscribers to appear on a topic
* `wait_for_service`: Wait for a service to become available
* `wait_for_action_server`: Wait for an action server to become available
* `wait_for_message`: Wait for a message, optionally with a predicate
* `wait_for_new_message`: Wait for any new message after the current count
* `call_service`: Call a service with automatic waiting and timeout handling
* `call_action`: Send an action goal with automatic waiting and result retrieval

### Assertions & Macros

* `ASSERT_SERVICE_EXISTS`: Assert that a service exists on the graph
* `EXPECT_SERVICE_EXISTS`: Expect that a service exists on the graph
* `ASSERT_ACTION_EXISTS`: Assert that an action server exists on the graph
* `EXPECT_ACTION_EXISTS`: Expect that an action server exists on the graph
* `ASSERT_SERVICE_EXISTS_WITH_EXECUTOR`: Assert service exists with custom executor
* `EXPECT_SERVICE_EXISTS_WITH_EXECUTOR`: Expect service exists with custom executor
* `ASSERT_ACTION_EXISTS_WITH_EXECUTOR`: Assert action exists with custom executor
* `EXPECT_ACTION_EXISTS_WITH_EXECUTOR`: Expect action exists with custom executor

### Utility Functions

* `node_options_from_yaml`: Load parameters from a YAML file into NodeOptions
* **LogCapture**: Helper to verify ROS log messages (singleton)


## Comparison with [rtest](https://github.com/Beam-and-Spyrosoft/rtest)

While **rtest** is an excellent tool for unit testing, it is limited by its design: it mocks `rclcpp` entirely. This makes it fast but unusable for code that relies on real middleware behavior or external libraries that manage their own ROS entities.

**hector_testing_utils** is designed to fill that gap. It provides a stable environment for the "general cases" where mocking isn't an option, aiming to make real integration tests as robust as possible.

| Feature | rtest | hector_testing_utils |
| --- | --- | --- |
| **Middleware** | **Mocked** (No DDS) | **Real** (Actual DDS/RMW) |
| **Execution Speed** | ⚡ Instant | 🐢 Slower |
| **Scope** | Unit Logic Only | Full Integration |
| **Limitations** | Cannot test complex library interactions | Subject to OS scheduling/timing |

**Summary:**

* **Use `rtest**` whenever possible for instant, flake-free feedback on logic.
* **Use `hector_testing_utils**` when `rtest` is too restrictive (e.g., testing launch files, complex node interactions, or opaque libraries).


## Basic Example

```cpp
#include <gtest/gtest.h>
#include <std_msgs/msg/int32.hpp>

#include <hector_testing_utils/hector_testing_utils.hpp>

using namespace std::chrono_literals;

TEST(Example, TestSubscription)
{
  auto node = std::make_shared<rclcpp::Node>("example_node");
  hector_testing_utils::TestExecutor executor;
  executor.add_node(node);

  const std::string topic = "/example/int32";
  auto pub = node->create_publisher<std_msgs::msg::Int32>(topic, 10);
  hector_testing_utils::TestSubscription<std_msgs::msg::Int32> sub(node, topic);

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

## Using Test Fixtures

The `HectorTestFixture` class provides a convenient base for tests with a pre-configured executor and test node:

```cpp
#include <gtest/gtest.h>
#include <hector_testing_utils/hector_testing_utils.hpp>

using namespace std::chrono_literals;
using hector_testing_utils::HectorTestFixture;

TEST_F(HectorTestFixture, SimplePublisherSubscriber)
{
  const std::string topic = "/test_topic";

  auto pub = tester_node_->create_test_publisher<std_msgs::msg::Int32>(topic);
  auto sub = tester_node_->create_test_subscription<std_msgs::msg::Int32>(topic);

  // Wait for all connections to be established
  ASSERT_TRUE(tester_node_->wait_for_all_connections(*executor_, 5s));

  std_msgs::msg::Int32 msg;
  msg.data = 100;
  pub->publish(msg);

  ASSERT_TRUE(sub->wait_for_message(*executor_, 5s));
  auto received = sub->last_message();
  ASSERT_TRUE(received.has_value());
  EXPECT_EQ(received->data, 100);
}
```

## TestNode Factory Methods

`TestNode` provides factory methods that automatically register connectables and enable connection tracking:

```cpp
TEST_F(HectorTestFixture, FactoryMethods)
{
  // Create publisher and subscriber with factory methods
  auto pub = tester_node_->create_test_publisher<std_msgs::msg::String>("/topic");
  auto sub = tester_node_->create_test_subscription<std_msgs::msg::String>("/topic");

  // Create service client and server
  auto client = tester_node_->create_test_client<example_interfaces::srv::AddTwoInts>("/service");
  auto server = tester_node_->create_test_service_server<example_interfaces::srv::AddTwoInts>(
    "/service",
    [](auto request, auto response) { response->sum = request->a + request->b; });

  // Wait for everything to connect
  ASSERT_TRUE(tester_node_->wait_for_all_connections(*executor_, 5s));
}
```

## Service Testing

```cpp
TEST_F(HectorTestFixture, ServiceTest)
{
  using Service = example_interfaces::srv::AddTwoInts;
  const std::string service_name = "/add_two_ints";

  auto server = tester_node_->create_test_service_server<Service>(
    service_name,
    [](const std::shared_ptr<Service::Request> request,
       std::shared_ptr<Service::Response> response) {
      response->sum = request->a + request->b;
    });

  auto client = tester_node_->create_test_client<Service>(service_name);

  ASSERT_TRUE(tester_node_->wait_for_all_connections(*executor_, 5s));
  ASSERT_SERVICE_EXISTS(tester_node_, service_name, 2s);

  auto request = std::make_shared<Service::Request>();
  request->a = 5;
  request->b = 7;

  auto future = client->get()->async_send_request(request);
  ASSERT_TRUE(executor_->spin_until_future_complete(future, 5s));
  auto response = future.get();
  ASSERT_NE(response, nullptr);
  EXPECT_EQ(response->sum, 12);
}
```

## Action Testing

```cpp
TEST_F(HectorTestFixture, ActionTest)
{
  using Fibonacci = example_interfaces::action::Fibonacci;
  using GoalHandle = rclcpp_action::ServerGoalHandle<Fibonacci>;
  const std::string action_name = "/fibonacci";

  auto handle_goal =
    [](const rclcpp_action::GoalUUID &, const std::shared_ptr<const Fibonacci::Goal>) {
      return rclcpp_action::GoalResponse::ACCEPT_AND_EXECUTE;
    };

  auto handle_cancel = [](const std::shared_ptr<GoalHandle>) {
      return rclcpp_action::CancelResponse::ACCEPT;
    };

  auto handle_accepted = [](const std::shared_ptr<GoalHandle> goal_handle) {
      auto result = std::make_shared<Fibonacci::Result>();
      result->sequence = {0, 1, 1, 2, 3};
      goal_handle->succeed(result);
    };

  auto server = tester_node_->create_test_action_server<Fibonacci>(
    action_name, handle_goal, handle_cancel, handle_accepted);
  auto client = tester_node_->create_test_action_client<Fibonacci>(action_name);

  ASSERT_TRUE(tester_node_->wait_for_all_connections(*executor_, 5s));
  ASSERT_ACTION_EXISTS(tester_node_, action_name, 2s);

  Fibonacci::Goal goal;
  goal.order = 5;

  auto goal_future = client->get()->async_send_goal(goal);
  ASSERT_TRUE(executor_->spin_until_future_complete(goal_future, 5s));
  auto goal_handle = goal_future.get();
  ASSERT_NE(goal_handle, nullptr);
}
```

## Scoped Context

If you need to avoid global `rclcpp::shutdown()` in shared test processes, use
`HectorTestFixtureWithContext` or manage a `TestContext` directly:

```cpp
class ScopedExample : public hector_testing_utils::HectorTestFixtureWithContext
{
  // Use tester_node_ and executor_ as usual.
};

TEST_F(ScopedExample, IsolatedTest)
{
  // Each test has its own isolated ROS 2 context
  auto pub = tester_node_->create_test_publisher<std_msgs::msg::Int32>("/topic");
  auto sub = tester_node_->create_test_subscription<std_msgs::msg::Int32>("/topic");

  ASSERT_TRUE(tester_node_->wait_for_all_connections(*executor_, 5s));
}
```

## Parameter Loading

Load parameters from YAML files into your test nodes:

```cpp
TEST(ParameterTest, LoadFromYaml)
{
  const std::string params_file = "path/to/params.yaml";
  auto options = hector_testing_utils::node_options_from_yaml(params_file);
  auto node = std::make_shared<rclcpp::Node>("param_node", options);

  int64_t my_param;
  ASSERT_TRUE(node->get_parameter("my_param", my_param));
}
```

## Message Predicates

Wait for specific messages using predicates:

```cpp
TEST_F(HectorTestFixture, PredicateWait)
{
  auto pub = tester_node_->create_test_publisher<std_msgs::msg::Int32>("/topic");
  auto sub = tester_node_->create_test_subscription<std_msgs::msg::Int32>("/topic");

  ASSERT_TRUE(tester_node_->wait_for_all_connections(*executor_, 5s));

  // Publish multiple messages
  for (int i = 1; i <= 10; ++i) {
    std_msgs::msg::Int32 msg;
    msg.data = i;
    pub->publish(msg);
  }

  // Wait for a message greater than 7
  ASSERT_TRUE(sub->wait_for_message(
    *executor_, 5s,
    [](const std_msgs::msg::Int32 &msg) { return msg.data > 7; }));
}
```

You can also wait for any new message without a predicate:

```cpp
// Wait for the next message after current state
ASSERT_TRUE(sub->wait_for_new_message(*executor_, 5s));
```

## Latched/Transient Local Messages

Test latched messages with transient local QoS:

```cpp
TEST_F(HectorTestFixture, LatchedMessage)
{
  const std::string topic = "/latched_topic";

  auto qos = rclcpp::QoS(rclcpp::KeepLast(1));
  qos.durability(RMW_QOS_POLICY_DURABILITY_TRANSIENT_LOCAL);
  qos.reliability(RMW_QOS_POLICY_RELIABILITY_RELIABLE);

  auto pub = tester_node_->create_publisher<std_msgs::msg::String>(topic, qos);

  std_msgs::msg::String msg;
  msg.data = "Latched message";
  pub->publish(msg);

  std::this_thread::sleep_for(100ms); // Allow DDS to persist

  // Create subscriber after publishing
  auto sub = tester_node_->create_test_subscription<std_msgs::msg::String>(
    topic, qos, true /* latched */);

  // Should receive the latched message
  ASSERT_TRUE(sub->wait_for_message(*executor_, 5s));
}
```

## Connection Diagnostics

Get diagnostic information about pending connections:

```cpp
TEST_F(HectorTestFixture, ConnectionDiagnostics)
{
  auto pub = tester_node_->create_test_publisher<std_msgs::msg::Int32>("/topic");
  auto sub = tester_node_->create_test_subscription<std_msgs::msg::Int32>("/topic");

  std::string diagnostic_report;
  bool connected = tester_node_->wait_for_all_connections(*executor_, 5s, &diagnostic_report);

  if (!connected) {
    RCLCPP_ERROR(tester_node_->get_logger(), "Failed to connect: %s", diagnostic_report.c_str());
  }
  ASSERT_TRUE(connected);
}
```

## Log Verification

You can verify that specific log messages (warnings, errors, etc.) were published using `LogCapture`. It intercepts ROS 2 logs via `rcutils`.

```cpp
TEST_F(HectorTestFixture, LogCheck)
{
  LogCapture capture; // Automatically registers log handler

  // Trigger something that logs
  RCLCPP_WARN(tester_node_->get_logger(), "Something happened!");

  // Assert log exists (regex support)
  ASSERT_TRUE(capture.wait_for_log(*executor_, "Something happened.*", 2s));
}
```

## Timeouts and Robustness

The helpers default to conservative timeouts (see `kDefaultTimeout` and the call option structs).
For slow CI pipelines, use longer timeouts instead of tight sleeps, and prefer the `wait_for_*`
helpers to avoid race conditions.

```cpp
// Custom timeout configuration
hector_testing_utils::ServiceCallOptions options;
options.service_timeout = 10s;
options.response_timeout = 10s;

auto response = hector_testing_utils::call_service<MyService>(
  client, request, executor, options);
```

## Call Helpers

Convenient helpers for calling services and actions:

```cpp
// Service call with automatic waiting
auto request = std::make_shared<AddTwoInts::Request>();
request->a = 1;
request->b = 2;

auto response = hector_testing_utils::call_service<AddTwoInts>(
  client, request, executor);

ASSERT_NE(response, nullptr);
EXPECT_EQ(response->sum, 3);

// Action call with automatic waiting
MyAction::Goal goal;
goal.target = 100;

auto result = hector_testing_utils::call_action<MyAction>(
  action_client, goal, executor);

ASSERT_TRUE(result.has_value());
EXPECT_EQ(result->code, rclcpp_action::ResultCode::SUCCEEDED);
```

## Advanced Executor Usage

 ### Custom Spinning
 The `TestExecutor` provides flexible spinning options:

 ```cpp
 TEST_F(HectorTestFixture, CustomSpinning)
 {
   // Spin until a condition is met
   bool result = executor_->spin_until(
     [this]() { return some_condition(); },
     5s
   );

   // Spin until a future completes
   auto future = client->async_send_request(request);
   ASSERT_TRUE(executor_->spin_until_future_complete(future, 5s));
 }
 ```

 ### Background Spinning

 Sometimes, especially when testing launch files or complex node interactions, you need the "System Under Test" to run continuously in the background while your test code performs checks nicely.

 Use `start_background_spinner()` to spawn a thread that spins the executor.

 ```cpp
 TEST_F(HectorTestFixture, BackgroundSpinning)
 {
   // Start spinning the nodes in a background thread
   executor_->start_background_spinner();

   // ... perform actions that require the system to be live ...

   // Use wait helpers (they also work with background spinner active!)
   ASSERT_TRUE(sub->wait_for_message(*executor_, 5s));

   // Stop spinning before tearing down (optional, destructor does it too)
   executor_->stop_background_spinner();
 }
 ```

 > [!NOTE]
 > `spin_until` and wait helpers automatically detect if the background spinner is active. If it is, they switch to "wait mode" (sleeping and checking predicate) instead of trying to spin the executor themselves.

 ### Single vs Multi-Threaded Executor

 By default, `HectorTestFixture` uses a `SingleThreadedExecutor`.

 **Use `SingleThreadedExecutor` (Default) when:**
 * You want deterministic, sequential execution.
 * Your callbacks are fast and non-blocking.
 * You want simpler debugging (no race conditions in your test nodes).

 **Use `MultiThreadedExecutor` when:**
 * You rely on Callback Groups for concurrent execution (e.g. Action Servers with parallel goal execution).
 * You have blocking callbacks (bad practice, but happens).
 * You want to simulate real deployment behavior more closely.

 **How to override:**

 ```cpp
 class MyMultiThreadedTest : public hector_testing_utils::HectorTestFixture
 {
 protected:
   std::shared_ptr<rclcpp::Executor> create_test_executor() override
   {
     return std::make_shared<rclcpp::executors::MultiThreadedExecutor>();
   }
 };
 ```

## Tips and Best Practices

1. **Use factory methods**: Prefer `create_test_publisher()` over `create_publisher()` to get automatic connection tracking
2. **Always wait for connections**: Use `wait_for_all_connections()` before publishing to avoid race conditions
3. **Use predicates**: Filter messages with predicates instead of checking values in a loop
4. **Set appropriate timeouts**: Use longer timeouts on CI systems; prefer waiting over sleeping
5. **Reset subscriptions**: Call `reset()` on subscriptions between test phases to clear cached messages
6. **Check diagnostics**: Use the diagnostic report parameter to debug connection issues
7. **Isolate contexts**: Use `HectorTestFixtureWithContext` when running tests in shared processes
8. **Match QoS policies**: Ensure publishers and subscribers use compatible QoS settings (reliability, durability) to avoid silent connection failures
9. **Use `wait_for_new_message()`**: When you need to wait for the next message regardless of content
10. **Leverage timing helpers**: Use the built-in timing and sequencing helpers for robust, deterministic tests

### Testing Lifecycle Nodes

When testing `LifecycleNodes`, remember that they do not automatically start. You must trigger their transitions:

```cpp
// Assuming 'my_node' is a LifecycleNode
auto state = my_node->configure();
ASSERT_EQ(state.id(), lifecycle_msgs::msg::State::PRIMARY_STATE_INACTIVE);

state = my_node->activate();
ASSERT_EQ(state.id(), lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE);

// Now your node is active and should be communicating
```

## Coverage Reporting

Generate coverage reports locally:

```bash
# Build with coverage enabled
colcon build --packages-select hector_testing_utils \
  --cmake-args -DENABLE_COVERAGE_TESTING=ON -DCMAKE_BUILD_TYPE=Debug

# Run tests
colcon test --packages-select hector_testing_utils

# Generate coverage report
cd build/hector_testing_utils
make hector_testing_utils_coverage

# View HTML report
xdg-open coverage_report/index.html
```

Or use the provided script:

```bash
cd /path/to/ros2_workspace
./src/hector_testing_utils/scripts/generate_coverage.sh
```

The CI pipeline automatically generates coverage reports and uploads them to [Codecov](https://codecov.io/gh/Joschi3/hector_testing_utils).

## Running Tests

Build and run the tests:

```bash
colcon build --packages-select hector_testing_utils
colcon test --packages-select hector_testing_utils
colcon test-result --verbose
```

View detailed test logs:

```bash
# Summary of all tests
colcon test-result --verbose

# Individual test logs
less log/latest_test/hector_testing_utils/stdout.log
less log/latest_test/hector_testing_utils/stderr.log
```

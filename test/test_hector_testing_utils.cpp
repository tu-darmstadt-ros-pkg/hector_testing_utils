#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include <example_interfaces/action/fibonacci.hpp>
#include <example_interfaces/srv/add_two_ints.hpp>
#include <std_msgs/msg/int32.hpp>

#include <rclcpp/rclcpp.hpp>
#include <rclcpp_action/rclcpp_action.hpp>

#include <hector_testing_utils/hector_testing_utils.hpp>

using namespace std::chrono_literals;

namespace
{

class RclcppFixture : public ::testing::Test
{
protected:
  static void SetUpTestSuite()
  {
    if (!rclcpp::ok()) {
      rclcpp::init(0, nullptr);
    }
  }

  static void TearDownTestSuite()
  {
    if (rclcpp::ok()) {
      rclcpp::shutdown();
    }
  }
};

}  // namespace

TEST_F(RclcppFixture, CachedSubscriberReceivesMessage)
{
  auto node = std::make_shared<rclcpp::Node>("cached_subscriber_test");
  hector_testing_utils::TestExecutor executor;
  executor.add_node(node);

  const std::string topic = "/cached_subscriber_test";
  auto pub = node->create_publisher<std_msgs::msg::Int32>(topic, 10);
  hector_testing_utils::CachedSubscriber<std_msgs::msg::Int32> sub(node, topic);

  ASSERT_TRUE(sub.wait_for_publishers(executor, 1, 10s));

  std_msgs::msg::Int32 msg;
  msg.data = 42;
  pub->publish(msg);

  ASSERT_TRUE(sub.wait_for_message(executor, 10s));
  auto last = sub.last_message();
  ASSERT_TRUE(last.has_value());
  EXPECT_EQ(last->data, 42);
}

TEST_F(RclcppFixture, CallServiceReturnsResponse)
{
  auto server_node = std::make_shared<rclcpp::Node>("add_two_ints_server");
  auto client_node = std::make_shared<rclcpp::Node>("add_two_ints_client");

  hector_testing_utils::TestExecutor executor;
  executor.add_node(server_node);
  executor.add_node(client_node);

  const std::string service_name = "/add_two_ints";
  auto service = server_node->create_service<example_interfaces::srv::AddTwoInts>(
    service_name,
    [](const std::shared_ptr<example_interfaces::srv::AddTwoInts::Request> request,
       std::shared_ptr<example_interfaces::srv::AddTwoInts::Response> response) {
      response->sum = request->a + request->b;
    });
  (void)service;

  auto client = client_node->create_client<example_interfaces::srv::AddTwoInts>(service_name);
  auto request = std::make_shared<example_interfaces::srv::AddTwoInts::Request>();
  request->a = 3;
  request->b = 4;

  auto response = hector_testing_utils::call_service<example_interfaces::srv::AddTwoInts>(
    client, request, executor);
  ASSERT_NE(response, nullptr);
  EXPECT_EQ(response->sum, 7);
}

TEST_F(RclcppFixture, CallActionReturnsResult)
{
  using Fibonacci = example_interfaces::action::Fibonacci;
  using GoalHandleFibonacci = rclcpp_action::ServerGoalHandle<Fibonacci>;

  auto server_node = std::make_shared<rclcpp::Node>("fibonacci_server");
  auto client_node = std::make_shared<rclcpp::Node>("fibonacci_client");

  hector_testing_utils::TestExecutor executor;
  executor.add_node(server_node);
  executor.add_node(client_node);

  const std::string action_name = "/fibonacci";

  auto handle_goal =
    [](const rclcpp_action::GoalUUID &, const std::shared_ptr<const Fibonacci::Goal> goal) {
      (void)goal;
      return rclcpp_action::GoalResponse::ACCEPT_AND_EXECUTE;
    };

  auto handle_cancel = [](const std::shared_ptr<GoalHandleFibonacci> goal_handle) {
      (void)goal_handle;
      return rclcpp_action::CancelResponse::ACCEPT;
    };

  auto handle_accepted = [](const std::shared_ptr<GoalHandleFibonacci> goal_handle) {
      std::thread([goal_handle]() {
        auto result = std::make_shared<Fibonacci::Result>();
        const int order = goal_handle->get_goal()->order;
        if (order <= 0) {
          result->sequence = {};
        } else if (order == 1) {
          result->sequence = {0};
        } else {
          result->sequence = {0, 1};
          for (int i = 2; i < order; ++i) {
            const int next = result->sequence[i - 1] + result->sequence[i - 2];
            result->sequence.push_back(next);
          }
        }
        goal_handle->succeed(result);
      }).detach();
    };

  auto server = rclcpp_action::create_server<Fibonacci>(
    server_node,
    action_name,
    handle_goal,
    handle_cancel,
    handle_accepted);
  (void)server;

  auto client = rclcpp_action::create_client<Fibonacci>(client_node, action_name);

  Fibonacci::Goal goal;
  goal.order = 5;

  auto wrapped = hector_testing_utils::call_action<example_interfaces::action::Fibonacci>(
    client, goal, executor);
  ASSERT_TRUE(wrapped.has_value());
  EXPECT_EQ(wrapped->code, rclcpp_action::ResultCode::SUCCEEDED);
  ASSERT_NE(wrapped->result, nullptr);
  EXPECT_EQ(wrapped->result->sequence.size(), 5u);
  EXPECT_EQ(wrapped->result->sequence[0], 0);
  EXPECT_EQ(wrapped->result->sequence[1], 1);
}

TEST_F(RclcppFixture, NodeOptionsFromYamlLoadsParameters)
{
  const std::string params_file = std::string(TEST_DATA_DIR) + "/test_params.yaml";
  auto options = hector_testing_utils::node_options_from_yaml(params_file);
  auto node = std::make_shared<rclcpp::Node>("param_node", options);

  int64_t foo = 0;
  std::string bar;
  bool use_sim_time = false;

  EXPECT_TRUE(node->get_parameter("foo", foo));
  EXPECT_TRUE(node->get_parameter("bar", bar));
  EXPECT_TRUE(node->get_parameter("use_sim_time", use_sim_time));

  EXPECT_EQ(foo, 42);
  EXPECT_EQ(bar, "baz");
  EXPECT_TRUE(use_sim_time);
}

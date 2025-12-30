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
using hector_testing_utils::HectorTestFixture;

TEST_F(HectorTestFixture, FactoryPublisherSubscriptionWithQoS)
{
  const std::string topic = "/qos_test_topic";

  // Custom QoS to ensure factory methods forward QoS settings.
  auto qos = rclcpp::QoS(rclcpp::KeepLast(1));
  qos.reliable();

  auto sub = tester_node_->create_test_subscription<std_msgs::msg::Int32>(topic, qos);
  auto pub = tester_node_->create_test_publisher<std_msgs::msg::Int32>(topic, qos);

  ASSERT_TRUE(tester_node_->wait_for_all_connections(*executor_, 5s));

  std_msgs::msg::Int32 msg;
  msg.data = 7;
  pub->publish(msg);

  ASSERT_TRUE(sub->wait_for_new_message(*executor_, 5s));
  auto last = sub->last_message();
  ASSERT_TRUE(last.has_value());
  EXPECT_EQ(last->data, 7);
}

TEST_F(HectorTestFixture, ServiceClientServerWaitsForConnections)
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

  ASSERT_TRUE(server->wait_for_client(*executor_, 5s));
  ASSERT_TRUE(client->wait_for_service(*executor_, 5s));
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

TEST_F(HectorTestFixture, ActionClientServerWaitsForConnections)
{
  using Fibonacci = example_interfaces::action::Fibonacci;
  using GoalHandle = rclcpp_action::ServerGoalHandle<Fibonacci>;
  const std::string action_name = "/fibonacci";

  auto handle_goal =
    [](const rclcpp_action::GoalUUID &, const std::shared_ptr<const Fibonacci::Goal> goal) {
      (void)goal;
      return rclcpp_action::GoalResponse::ACCEPT_AND_EXECUTE;
    };

  auto handle_cancel = [](const std::shared_ptr<GoalHandle> goal_handle) {
      (void)goal_handle;
      return rclcpp_action::CancelResponse::ACCEPT;
    };

  auto handle_accepted = [](const std::shared_ptr<GoalHandle> goal_handle) {
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

  auto server = tester_node_->create_test_action_server<Fibonacci>(
    action_name, handle_goal, handle_cancel, handle_accepted);
  auto client = tester_node_->create_test_action_client<Fibonacci>(action_name);

  ASSERT_TRUE(server->wait_for_client(*executor_, 5s));
  ASSERT_TRUE(client->wait_for_server(*executor_, 5s));
  ASSERT_TRUE(tester_node_->wait_for_all_connections(*executor_, 5s));
  ASSERT_ACTION_EXISTS(tester_node_, action_name, 2s);

  Fibonacci::Goal goal;
  goal.order = 5;

  auto goal_future = client->get()->async_send_goal(goal);
  ASSERT_TRUE(executor_->spin_until_future_complete(goal_future, 5s));
  auto goal_handle = goal_future.get();
  ASSERT_NE(goal_handle, nullptr);

  auto result_future = client->get()->async_get_result(goal_handle);
  ASSERT_TRUE(executor_->spin_until_future_complete(result_future, 10s));
  auto wrapped = result_future.get();

  ASSERT_EQ(wrapped.code, rclcpp_action::ResultCode::SUCCEEDED);
  ASSERT_NE(wrapped.result, nullptr);
  EXPECT_EQ(wrapped.result->sequence.size(), 5u);
  EXPECT_EQ(wrapped.result->sequence.back(), 3);
}

TEST_F(HectorTestFixture, CachedSubscriberReceivesMessage)
{
  auto subscriber_node = std::make_shared<rclcpp::Node>("cached_subscriber_test_node");
  executor_->add_node(subscriber_node);

  const std::string topic = "/cached_subscriber_test";
  auto pub = tester_node_->create_publisher<std_msgs::msg::Int32>(topic, rclcpp::QoS(5).best_effort());
  hector_testing_utils::CachedSubscriber<std_msgs::msg::Int32> sub(subscriber_node, topic, rclcpp::QoS(5));

  ASSERT_TRUE(sub.wait_for_publishers(*executor_, 1, 5s));

  std_msgs::msg::Int32 msg;
  msg.data = 42;
  pub->publish(msg);

  ASSERT_TRUE(sub.wait_for_message(*executor_, 5s));
  auto last = sub.last_message();
  ASSERT_TRUE(last.has_value());
  EXPECT_EQ(last->data, 42);
}

TEST_F(HectorTestFixture, NodeOptionsFromYamlLoadsParameters)
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

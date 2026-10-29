#include <chrono>
#include <memory>

#include <gtest/gtest.h>

#include <example_interfaces/action/fibonacci.hpp>
#include <example_interfaces/srv/add_two_ints.hpp>
#include <std_msgs/msg/int32.hpp>

#include <rclcpp/rclcpp.hpp>

#include <hector_testing_utils/hector_testing_utils.hpp>

using namespace std::chrono_literals;
using hector_testing_utils::HectorTestFixtureWithContext;

// Test that HectorTestFixtureWithContext properly initializes and tears down context
TEST_F( HectorTestFixtureWithContext, ContextInitialization )
{
  ASSERT_NE( context_, nullptr );
  ASSERT_NE( executor_, nullptr );
  ASSERT_NE( tester_node_, nullptr );

  auto ctx = context_->context();
  ASSERT_NE( ctx, nullptr );
  EXPECT_TRUE( ctx->is_valid() );
}

// Test basic pub/sub with scoped context
TEST_F( HectorTestFixtureWithContext, BasicPubSubWithContext )
{
  const std::string topic = "/context_test/pubsub";

  auto pub = tester_node_->create_test_publisher<std_msgs::msg::Int32>( topic );
  auto sub = tester_node_->create_test_subscription<std_msgs::msg::Int32>( topic );

  ASSERT_TRUE( tester_node_->wait_for_all_connections( *executor_, 5s ) );

  std_msgs::msg::Int32 msg;
  msg.data = 42;
  pub->publish( msg );

  ASSERT_TRUE( sub->wait_for_message( *executor_, 5s ) );
  auto received = sub->last_message();
  ASSERT_TRUE( received.has_value() );
  EXPECT_EQ( received->data, 42 );
}

// Test creating additional nodes with the same context
TEST_F( HectorTestFixtureWithContext, MultipleNodesSharedContext )
{
  auto node1 = std::make_shared<rclcpp::Node>( "node1", context_->node_options() );
  auto node2 = std::make_shared<rclcpp::Node>( "node2", context_->node_options() );

  executor_->add_node( node1 );
  executor_->add_node( node2 );

  const std::string topic = "/context_test/multi_node";

  auto pub = node1->create_publisher<std_msgs::msg::Int32>( topic, 10 );
  hector_testing_utils::TestSubscription<std_msgs::msg::Int32> sub( node2, topic );

  ASSERT_TRUE( sub.wait_for_publishers( *executor_, 1, 5s ) );

  std_msgs::msg::Int32 msg;
  msg.data = 123;
  pub->publish( msg );

  ASSERT_TRUE( sub.wait_for_message( *executor_, 5s ) );
  auto received = sub.last_message();
  ASSERT_TRUE( received.has_value() );
  EXPECT_EQ( received->data, 123 );
}

// Test service client/server with scoped context
TEST_F( HectorTestFixtureWithContext, ServiceWithContext )
{
  using Service = example_interfaces::srv::AddTwoInts;
  const std::string service_name = "/context_test/service";

  auto server = tester_node_->create_test_service_server<Service>(
      service_name, []( const std::shared_ptr<Service::Request> request,
                        std::shared_ptr<Service::Response> response ) {
        response->sum = request->a + request->b;
      } );

  auto client = tester_node_->create_test_client<Service>( service_name );

  ASSERT_TRUE( tester_node_->wait_for_all_connections( *executor_, 5s ) );

  auto request = std::make_shared<Service::Request>();
  request->a = 10;
  request->b = 20;

  auto future = client->get()->async_send_request( request );
  ASSERT_TRUE( executor_->spin_until_future_complete( future, 5s ) );
  auto response = future.get();
  ASSERT_NE( response, nullptr );
  EXPECT_EQ( response->sum, 30 );
}

// Test context isolation - manually creating a TestContext
TEST( ContextIsolation, ManualContextCreation )
{
  // Create a scoped context
  hector_testing_utils::TestContext ctx1;
  hector_testing_utils::TestExecutor exec1( ctx1.context() );

  auto node1 = std::make_shared<rclcpp::Node>( "isolated_node", ctx1.node_options() );
  exec1.add_node( node1 );

  EXPECT_TRUE( ctx1.context()->is_valid() );
  EXPECT_TRUE( rclcpp::ok( ctx1.context() ) );

  // Create a second independent context
  hector_testing_utils::TestContext ctx2;
  hector_testing_utils::TestExecutor exec2( ctx2.context() );

  auto node2 = std::make_shared<rclcpp::Node>( "isolated_node", ctx2.node_options() );
  exec2.add_node( node2 );

  EXPECT_TRUE( ctx2.context()->is_valid() );
  EXPECT_TRUE( rclcpp::ok( ctx2.context() ) );

  // Both contexts should be independent
  EXPECT_NE( ctx1.context(), ctx2.context() );
}

// Test parameter loading with context
TEST_F( HectorTestFixtureWithContext, ParameterLoadingWithContext )
{
  const std::string params_file = std::string( TEST_DATA_DIR ) + "/test_params.yaml";

  auto options = hector_testing_utils::node_options_from_yaml( params_file );
  options.context( context_->context() );

  auto param_node = std::make_shared<rclcpp::Node>( "param_node", options );
  executor_->add_node( param_node );

  int64_t foo = 0;
  std::string bar;
  bool use_sim_time = false;

  EXPECT_TRUE( param_node->get_parameter( "foo", foo ) );
  EXPECT_TRUE( param_node->get_parameter( "bar", bar ) );
  EXPECT_TRUE( param_node->get_parameter( "use_sim_time", use_sim_time ) );

  EXPECT_EQ( foo, 42 );
  EXPECT_EQ( bar, "baz" );
  EXPECT_TRUE( use_sim_time );
}

// Test that context is properly cleaned up after test
TEST_F( HectorTestFixtureWithContext, ContextCleanup )
{
  // Store the context pointer
  auto ctx = context_->context();
  ASSERT_TRUE( ctx->is_valid() );

  // Create and publish a message
  const std::string topic = "/cleanup_test";
  auto pub = tester_node_->create_test_publisher<std_msgs::msg::Int32>( topic );

  std_msgs::msg::Int32 msg;
  msg.data = 999;
  pub->publish( msg );

  // Spin a bit
  executor_->spin_some();

  // Context should still be valid during test
  EXPECT_TRUE( ctx->is_valid() );
  EXPECT_TRUE( rclcpp::ok( ctx ) );

  // TearDown will be called automatically after this test
  // and should properly shut down the context
}

// Test that wrappers registered with the node don't keep it alive
TEST_F( HectorTestFixtureWithContext, NodeReleasedWithRegisteredWrappers )
{
  using Service = example_interfaces::srv::AddTwoInts;
  using Action = example_interfaces::action::Fibonacci;

  tester_node_->create_test_publisher<std_msgs::msg::Int32>( "/release_test/topic" );
  tester_node_->create_test_subscription<std_msgs::msg::Int32>( "/release_test/topic" );
  tester_node_->create_test_client<Service>( "/release_test/service" );
  tester_node_->create_test_service_server<Service>(
      "/release_test/service",
      []( const std::shared_ptr<Service::Request>, std::shared_ptr<Service::Response> ) { } );
  tester_node_->create_test_action_client<Action>( "/release_test/action" );
  tester_node_->create_test_action_server<Action>(
      "/release_test/action",
      []( const rclcpp_action::GoalUUID &, std::shared_ptr<const Action::Goal> ) {
        return rclcpp_action::GoalResponse::REJECT;
      },
      []( const std::shared_ptr<rclcpp_action::ServerGoalHandle<Action>> ) {
        return rclcpp_action::CancelResponse::REJECT;
      },
      []( const std::shared_ptr<rclcpp_action::ServerGoalHandle<Action>> ) {} );

  std::weak_ptr<hector_testing_utils::TestNode> weak_node = tester_node_;
  tester_node_.reset();
  executor_.reset();

  EXPECT_TRUE( weak_node.expired() );
}

// Test multiple test fixture instances don't interfere
class ContextTest1 : public HectorTestFixtureWithContext
{
};
class ContextTest2 : public HectorTestFixtureWithContext
{
};

TEST_F( ContextTest1, FirstInstance )
{
  const std::string topic = "/context_instance_1";
  auto pub = tester_node_->create_test_publisher<std_msgs::msg::Int32>( topic );
  auto sub = tester_node_->create_test_subscription<std_msgs::msg::Int32>( topic );

  ASSERT_TRUE( tester_node_->wait_for_all_connections( *executor_, 5s ) );

  std_msgs::msg::Int32 msg;
  msg.data = 100;
  pub->publish( msg );

  ASSERT_TRUE( sub->wait_for_message( *executor_, 5s ) );
  auto received = sub->last_message();
  ASSERT_TRUE( received.has_value() );
  EXPECT_EQ( received->data, 100 );
}

TEST_F( ContextTest2, SecondInstance )
{
  const std::string topic = "/context_instance_2";
  auto pub = tester_node_->create_test_publisher<std_msgs::msg::Int32>( topic );
  auto sub = tester_node_->create_test_subscription<std_msgs::msg::Int32>( topic );

  ASSERT_TRUE( tester_node_->wait_for_all_connections( *executor_, 5s ) );

  std_msgs::msg::Int32 msg;
  msg.data = 200;
  pub->publish( msg );

  ASSERT_TRUE( sub->wait_for_message( *executor_, 5s ) );
  auto received = sub->last_message();
  ASSERT_TRUE( received.has_value() );
  EXPECT_EQ( received->data, 200 );
}

// Test executor construction with context
TEST( TestExecutorWithContext, ExecutorConstruction )
{
  hector_testing_utils::TestContext ctx;
  hector_testing_utils::TestExecutor executor( ctx.context() );

  auto node = std::make_shared<rclcpp::Node>( "test_exec_node", ctx.node_options() );
  executor.add_node( node );

  // Test spin_until with simple predicate
  int counter = 0;
  std::thread increment( [&counter]() {
    std::this_thread::sleep_for( 100ms );
    counter = 5;
  } );

  bool result = executor.spin_until( [&counter]() { return counter == 5; }, 2s );
  increment.join();

  EXPECT_TRUE( result );
  EXPECT_EQ( counter, 5 );
}

// Test TestSubscription with context
TEST_F( HectorTestFixtureWithContext, TestSubscriptionWithContext )
{
  auto sub_node = std::make_shared<rclcpp::Node>( "cached_sub_node", context_->node_options() );
  executor_->add_node( sub_node );

  const std::string topic = "/context_cached_sub";

  auto pub = tester_node_->create_publisher<std_msgs::msg::Int32>( topic, 10 );
  hector_testing_utils::TestSubscription<std_msgs::msg::Int32> sub( sub_node, topic );

  ASSERT_TRUE( sub.wait_for_publishers( *executor_, 1, 5s ) );

  std_msgs::msg::Int32 msg;
  msg.data = 777;
  pub->publish( msg );

  ASSERT_TRUE( sub.wait_for_message( *executor_, 5s ) );
  auto received = sub.last_message();
  ASSERT_TRUE( received.has_value() );
  EXPECT_EQ( received->data, 777 );
}

// Test node_options_from_yaml with context
TEST_F( HectorTestFixtureWithContext, NodeOptionsFromYamlWithContext )
{
  const std::string params_file = std::string( TEST_DATA_DIR ) + "/test_params.yaml";

  auto options = hector_testing_utils::node_options_from_yaml( params_file );
  options.context( context_->context() );

  // Use "param_node" to match the namespace in test_params.yaml
  auto node = std::make_shared<rclcpp::Node>( "param_node", options );

  int64_t foo = 0;
  ASSERT_TRUE( node->get_parameter( "foo", foo ) );
  EXPECT_EQ( foo, 42 );

  executor_->add_node( node );
  executor_->spin_some(); // Should not crash
}

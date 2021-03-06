/* Copyright (c) 2017, United States Government, as represented by the
 * Administrator of the National Aeronautics and Space Administration.
 * 
 * All rights reserved.
 * 
 * The Astrobee platform is licensed under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with the
 * License. You may obtain a copy of the License at
 * 
 *     http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */

// Command line flags
#include <gflags/gflags.h>
#include <gflags/gflags_completions.h>

// Include RPOS
#include <ros/ros.h>

// Listen for transforms
#include <tf2_ros/transform_listener.h>

// FSW includes
#include <ff_util/ff_names.h>
#include <ff_util/ff_flight.h>
#include <ff_util/ff_action.h>
#include <ff_util/ff_serialization.h>
#include <ff_util/config_client.h>

// Primitive actions
#include <ff_msgs/SwitchAction.h>
#include <ff_msgs/MotionAction.h>

// Eigen C++ includes
#include <Eigen/Dense>
#include <Eigen/Geometry>

// C++ STL inclues
#include <iostream>
#include <sstream>
#include <fstream>
#include <string>
#include <memory>

// Gflags
DEFINE_string(ns, "", "Robot namespace");
DEFINE_string(loc, "", "Localization pipeline (none, ml, ar, hr)");
DEFINE_string(mode, "", "Flight mode");
DEFINE_string(planner, "trapezoidal", "Path planning algorithm");
DEFINE_bool(ff, false, "Plan in face-forward mode");
DEFINE_double(rate, 1.0, "Segment sampling rate");
DEFINE_double(vel, -1.0, "Desired velocity");
DEFINE_double(accel, -1.0, "Desired acceleration");
DEFINE_double(omega, -1.0, "Desired angular velocity");
DEFINE_double(alpha, -1.0, "Desired angular acceleration");
DEFINE_bool(move, false, "Send move command");
DEFINE_bool(stop, false, "Send stop command");
DEFINE_bool(idle, false, "Send idle command");
DEFINE_bool(prep, false, "Send prep command");
DEFINE_bool(novalidate, false, "Don't validate the segment before running");
DEFINE_bool(nocollision, false, "Don't check for collisions during action");
DEFINE_bool(nobootstrap, false, "Don't move to the starting station on execute");
DEFINE_bool(noimmediate, false, "Don't execute immediately");
DEFINE_bool(replan, false, "Enable replanning");
DEFINE_bool(timesync, false, "Enable time synchronization");
DEFINE_string(rec, "", "Plan and record to this file.");
DEFINE_string(exec, "", "Execute a given segment");
DEFINE_string(pos, "", "Desired position in cartesian format 'X Y Z' (meters)");
DEFINE_string(att, "", "Desired attitude in angle-axis format 'angle X Y Z'");
DEFINE_double(wait, 0.0, "Defer move by given amount in seconds (needs -noimmediate)");
DEFINE_double(connect, 30.0, "Action connect timeout");
DEFINE_double(active, 30.0, "Action active timeout");
DEFINE_double(response, 30.0, "Action response timeout");
DEFINE_double(deadline, -1.0, "Action deadline timeout");

// Avoid sending the command multiple times
bool sent_ = false;

// Generic completion function
void MResultCallback(ff_util::FreeFlyerActionState::Enum result_code,
  ff_msgs::MotionResultConstPtr const& result) {
  switch (result_code) {
  // Result will be a null pointer
  case ff_util::FreeFlyerActionState::Enum::TIMEOUT_ON_CONNECT:
    std::cout << "Timeout on connecting to action" << std::endl;
    break;
  case ff_util::FreeFlyerActionState::Enum::TIMEOUT_ON_ACTIVE:
    std::cout << "Timeout on action going active" << std::endl;
    break;
  case ff_util::FreeFlyerActionState::Enum::TIMEOUT_ON_RESPONSE:
    std::cout << "Timeout on receiving a response" << std::endl;
    break;
  case ff_util::FreeFlyerActionState::Enum::TIMEOUT_ON_DEADLINE:
    std::cout << "Timeout on result deadline" << std::endl;
    break;
  // Result expected
  case ff_util::FreeFlyerActionState::Enum::SUCCESS:
    if (!FLAGS_rec.empty()) {
      ff_msgs::MotionGoal msg;
      msg.command = ff_msgs::MotionGoal::EXEC;
      msg.flight_mode = FLAGS_mode;
      msg.segment = result->segment;
      if (!ff_util::Serialization::WriteFile(FLAGS_rec, msg))
        std::cout << std::endl << "Segment saved to " << FLAGS_rec;
      else
        std::cout << std::endl << "Segment not saved";
    }
  case ff_util::FreeFlyerActionState::Enum::PREEMPTED:
  case ff_util::FreeFlyerActionState::Enum::ABORTED: {
    // Print a meningful response
    std::cout << std::endl << "Result: ";
    switch (result->response) {
    case ff_msgs::MotionResult::ALREADY_THERE:
      std::cout << "We are already at the location" << std::endl;       break;
    case ff_msgs::MotionResult::SUCCESS:
      std::cout << "Motion succeeded" << std::endl;                     break;
    case ff_msgs::MotionResult::CANCELLED:
      std::cout << "Motion cancelled by callee" << std::endl;           break;
    case ff_msgs::MotionResult::PREEMPTED:
      std::cout << "Motion preempted by thirdparty" << std::endl;       break;
    case ff_msgs::MotionResult::PLAN_FAILED:
      std::cout << "Plan/bootstrap failed" << std::endl;                break;
    case ff_msgs::MotionResult::VALIDATE_FAILED:
      std::cout << "Validate failed" << std::endl;                      break;
    case ff_msgs::MotionResult::CONTROL_FAILED:
      std::cout << "Control failed" << std::endl;                       break;
    case ff_msgs::MotionResult::OBSTACLE_DETECTED:
      std::cout << "Obstacle detected / replan disabled" << std::endl;  break;
    case ff_msgs::MotionResult::REPLAN_NOT_ENOUGH_TIME:
      std::cout << "Obstacle and no time to replan" << std::endl;       break;
    case ff_msgs::MotionResult::REPLAN_FAILED:
      std::cout << "Obstacle and replanning failed" << std::endl;       break;
    case ff_msgs::MotionResult::REVALIDATE_FAILED:
      std::cout << "Obstacle and revalidating failed" << std::endl;     break;
    case ff_msgs::MotionResult::NOT_IN_WAITING_MODE:
      std::cout << "Internal failure" << std::endl;                     break;
    case ff_msgs::MotionResult::INVALID_FLIGHT_MODE:
      std::cout << "Invalid flight mode specified" << std::endl;        break;
    case ff_msgs::MotionResult::UNEXPECTED_EMPTY_SEGMENT:
      std::cout << "Segment empty" << std::endl;                        break;
    case ff_msgs::MotionResult::COULD_NOT_RESAMPLE:
      std::cout << "Could not resample segment" << std::endl;           break;
    case ff_msgs::MotionResult::UNEXPECTED_EMPTY_STATES:
      std::cout << "State vector empty" << std::endl;                   break;
    case ff_msgs::MotionResult::INVALID_COMMAND:
      std::cout << "Command rejected" << std::endl;                     break;
    case ff_msgs::MotionResult::CANNOT_QUERY_ROBOT_POSE:
      std::cout << "Failed to find the current pose" << std::endl;      break;
    case ff_msgs::MotionResult::NOT_ON_FIRST_POSE:
      std::cout << "Not on first pose / no bootstrapping" << std::endl; break;
    case ff_msgs::MotionResult::BAD_DESIRED_VELOCITY:
      std::cout << "Requested vel too high" << std::endl;               break;
    case ff_msgs::MotionResult::BAD_DESIRED_ACCELERATION:
      std::cout << "Requested accel too high" << std::endl;             break;
    case ff_msgs::MotionResult::BAD_DESIRED_OMEGA:
      std::cout << "Requested omega too high" << std::endl;             break;
    case ff_msgs::MotionResult::BAD_DESIRED_ALPHA:
      std::cout << "Requested alpha too high" << std::endl;             break;
    case ff_msgs::MotionResult::BAD_DESIRED_RATE:
      std::cout << "Requested rate too low" << std::endl;               break;
    case ff_msgs::MotionResult::TOLERANCE_VIOLATION_POSITION:
      std::cout << "Position tolerance violated" << std::endl;          break;
    case ff_msgs::MotionResult::TOLERANCE_VIOLATION_ATTITUDE:
      std::cout << "Attitude tolerance violated" << std::endl;          break;
    case ff_msgs::MotionResult::TOLERANCE_VIOLATION_VELOCITY:
      std::cout << "Velocity tolerance violated" << std::endl;          break;
    case ff_msgs::MotionResult::TOLERANCE_VIOLATION_OMEGA:
      std::cout << "Omega tolerance violated" << std::endl;             break;
    default:
      std::cout << "Error: unknown" << std::endl;                       break;
    }
  }
  default:
    break;
  }
  ros::shutdown();
}

// Mobility feedback
void MFeedbackCallback(ff_msgs::MotionFeedbackConstPtr const& feedback) {
  std::string str = "UNKNOWN";
  switch (feedback->state.state) {
  case ff_msgs::MotionState::INITIALIZING:     str = "INITIALIZING";     break;
  case ff_msgs::MotionState::WAITING_FOR_STOP: str = "WAITING_FOR_STOP"; break;
  case ff_msgs::MotionState::WAITING:          str = "WAITING";          break;
  case ff_msgs::MotionState::IDLING:           str = "IDLING";           break;
  case ff_msgs::MotionState::STOPPING:         str = "STOPPING";         break;
  case ff_msgs::MotionState::PREPPING:         str = "PREPPING";         break;
  case ff_msgs::MotionState::BOOTSTRAPPING:    str = "BOOTSTRAPPING";    break;
  case ff_msgs::MotionState::PLANNING:         str = "PLANNING";         break;
  case ff_msgs::MotionState::VALIDATING:       str = "VALIDATING";       break;
  case ff_msgs::MotionState::PREPARING:        str = "PREPARING";        break;
  case ff_msgs::MotionState::CONTROLLING:      str = "CONTROLLING";      break;
  case ff_msgs::MotionState::REPLANNING:       str = "REPLANNING";       break;
  case ff_msgs::MotionState::REVALIDATING:     str = "REVALIDATING";     break;
  }
  std::cout << '\r' << std::flush;
  std::cout << std::fixed << std::setprecision(2)
    << "POS: " << 1000.00 * feedback->progress.error_position << " mm "
    << "ATT: " << 57.2958 * feedback->progress.error_attitude << " deg "
    << "VEL: " << 1000.00 * feedback->progress.error_velocity << " mm/s "
    << "OMEGA: " << 57.2958 * feedback->progress.error_omega << " deg/s "
    << "[" << str << "]           ";
}

// Switch feedback
void SFeedbackCallback(ff_msgs::SwitchFeedbackConstPtr const& feedback) {}

// Switch result
void SResultCallback(ff_util::FreeFlyerActionState::Enum result_code,
  ff_msgs::SwitchResultConstPtr const& result,
  tf2_ros::Buffer * tf_buffer_,
  ff_util::FreeFlyerActionClient<ff_msgs::MotionAction> * action) {
  // Setup a new mobility goal
  ff_msgs::MotionGoal goal;
  goal.flight_mode = FLAGS_mode;
  // Rest of the goal depends on result
  switch (result_code) {
  case ff_util::FreeFlyerActionState::SUCCESS: {
    // Idle command
    if (FLAGS_idle)
      goal.command = ff_msgs::MotionGoal::IDLE;
    // Stop command
    if (FLAGS_stop)
      goal.command = ff_msgs::MotionGoal::STOP;
    // Stop command
    if (FLAGS_prep)
      goal.command = ff_msgs::MotionGoal::PREP;
    // Obtain the current state
    if (FLAGS_move) {
      goal.command = ff_msgs::MotionGoal::MOVE;
      geometry_msgs::PoseStamped state;
      try {
        std::string ns = FLAGS_ns;
        geometry_msgs::TransformStamped tfs = tf_buffer_->lookupTransform(
          std::string(FRAME_NAME_WORLD),
          (ns.empty() ? "body" : ns + "/" + std::string(FRAME_NAME_BODY)),
          ros::Time(0));
        state.header = tfs.header;
        state.pose.position.x = tfs.transform.translation.x;
        state.pose.position.y = tfs.transform.translation.y;
        state.pose.position.z = tfs.transform.translation.z;
        state.pose.orientation = tfs.transform.rotation;
      } catch (tf2::TransformException &ex) {
        std::cout << "Could not query the pose of the robot: "
                  << ex.what() << std::endl;
        ros::shutdown();
      }
      // Manipulate timestamp to cause deferral
      state.header.stamp += ros::Duration(FLAGS_wait);
      // Parse and modify the position
      std::string str_p = FLAGS_pos;
      if (!str_p.empty()) {
        std::istringstream iss_p(str_p);
        std::vector<double> vec_p {
          std::istream_iterator<double>(iss_p),
          std::istream_iterator<double>()
        };
        if (vec_p.size() > 0) state.pose.position.x = vec_p[0];
        if (vec_p.size() > 1) state.pose.position.y = vec_p[1];
        if (vec_p.size() > 2) state.pose.position.z = vec_p[2];
      }
      // Parse the attitude - roll, pitch then yaw
      std::string str_a = FLAGS_att;
      if (!str_a.empty()) {
        // Parse double vector from string
        std::istringstream iss_a(str_a);
        std::vector<double> vec_a {
          std::istream_iterator<double>(iss_a),
          std::istream_iterator<double>()
        };
        // Convert the axis angle input to a quaternion
        Eigen::AngleAxisd aa(0.0, Eigen::Vector3d(0.0, 0.0, 0.0));
        if (vec_a.size() == 1) {
          Eigen::Quaterniond q0(state.pose.orientation.w, state.pose.orientation.x,
            state.pose.orientation.y, state.pose.orientation.z);
          Eigen::Vector3d x(1, 0, 0);
          Eigen::Vector3d p = q0.matrix()*x;
          p(2) = 0;
          p.normalize();
          double alpha = vec_a[0] - std::atan2(p(1), p(0));
          Eigen::Quaterniond qz(std::cos(0.5*alpha), 0, 0, std::sin(0.5*alpha));
          Eigen::Quaterniond qd = qz*q0;
          Eigen::Vector3d p_check = qd.matrix()*x;
          p_check(2) = 0;
          Eigen::Vector3d p_check2(std::cos(alpha), std::sin(alpha), 0);
          // End check
          state.pose.orientation.x = qd.x();
          state.pose.orientation.y = qd.y();
          state.pose.orientation.z = qd.z();
          state.pose.orientation.w = qd.w();
        } else if (vec_a.size() == 4) {
          aa.angle() = vec_a[0];
          aa.axis().x() = vec_a[1];
          aa.axis().y() = vec_a[2];
          aa.axis().z() = vec_a[3];
          Eigen::Quaterniond q(aa);
          state.pose.orientation.x = q.x();
          state.pose.orientation.y = q.y();
          state.pose.orientation.z = q.z();
          state.pose.orientation.w = q.w();
        } else if (vec_a.size() > 0) {
          std::cout << "Invalid axis-angle format passed to -att. "
            << "Four elements required. Aborting" << std::endl;
          break;
        }
      }
      // Package up and send the move goal
      goal.states.push_back(state);
    }
    // Execute command
    if (!FLAGS_exec.empty()) {
      if (!ff_util::Serialization::ReadFile(FLAGS_exec, goal)) {
        std::cout << "Segment not loaded from file " << FLAGS_exec << std::endl;
        break;
      }
    }
    // Try and send the goal
    if (!action->SendGoal(goal))
      std::cout << "Mobility client did not accept goal" << std::endl;
    else
      return;
  }
  case ff_util::FreeFlyerActionState::PREEMPTED:
    std::cout << "Error: PREEMPTED" << std::endl;
    break;
  case ff_util::FreeFlyerActionState::ABORTED:
    std::cout << "Error: ABORTED" << std::endl;
    break;
  case ff_util::FreeFlyerActionState::TIMEOUT_ON_CONNECT:
    std::cout << "Error: TIMEOUT_ON_CONNECT" << std::endl;
    break;
  case ff_util::FreeFlyerActionState::TIMEOUT_ON_ACTIVE:
    std::cout << "Error: TIMEOUT_ON_ACTIVE" << std::endl;
    break;
  case ff_util::FreeFlyerActionState::TIMEOUT_ON_RESPONSE:
    std::cout << "Error: TIMEOUT_ON_RESPONSE" << std::endl;
    break;
  case ff_util::FreeFlyerActionState::TIMEOUT_ON_DEADLINE:
    std::cout << "Error: TIMEOUT_ON_DEADLINE" << std::endl;
    break;
  default:
    std::cout << "Error: UNKNOWN" << std::endl;
    break;
  }
  ros::shutdown();
}

// Ensure all clients are connected
void ConnectedCallback(tf2_ros::Buffer * tf_buffer_,
  ff_util::FreeFlyerActionClient<ff_msgs::SwitchAction> * client_s_,
  ff_util::FreeFlyerActionClient<ff_msgs::MotionAction> * client_t_) {
  // Check to see if connected
  if (!client_s_->IsConnected()) return;  // Switch
  if (!client_t_->IsConnected()) return;  // Mobility
  if (sent_)                     return;  // Avoid calling twice
  else
    sent_ = true;
  // Debug
  std::cout << "All actions connected. Sending command..." << std::endl;
  // Package up and send the move goal
  if (!FLAGS_loc.empty()) {
    ff_msgs::SwitchGoal switch_goal;
    switch_goal.pipeline = FLAGS_loc;
    if (!client_s_->SendGoal(switch_goal))
      std::cout << "Switch client did not accept goal" << std::endl;
    return;
  }
  // Fake a switch result to trigger the releop action
  SResultCallback(ff_util::FreeFlyerActionState::SUCCESS, nullptr,
    tf_buffer_, client_t_);
}

// Main entry point for application
int main(int argc, char *argv[]) {
  // Initialize a ros node
  ros::init(argc, argv, "teleop", ros::init_options::AnonymousName);
  // Gather some data from the command
  google::SetUsageMessage("Usage: rosrun mobility teleop <opts>");
  google::SetVersionString("1.0.0");
  google::ParseCommandLineFlags(&argc, &argv, true);
  // Some simple checks
  uint8_t mode = 0;
  if (!FLAGS_exec.empty()) mode++;
  if (FLAGS_idle) mode++;
  if (FLAGS_stop) mode++;
  if (FLAGS_move) mode++;
  if (FLAGS_prep) mode++;
  // Check we have specified one of the required switches
  if (FLAGS_loc.empty() && mode == 0) {
    std::cout << "You must specify one of "
      << "-loc, -move, -stop, -idle, -exec <segment>" << std::endl;
    return 1;
  }
  if (mode > 1) {
    std::cout << "You can only specify one of "
      << "-move, -stop, -idle, or -exec <segment>" << std::endl;
    return 1;
  }
  if (FLAGS_connect <= 0.0) {
    std::cout << "Your connect timeout must be positive" << std::endl;
    return 1;
  }
  if (FLAGS_active <= 0.0) {
    std::cout << "Your active timeout must be positive" << std::endl;
    return 1;
  }
  if (FLAGS_response <= 0.0) {
    std::cout << "Your response timeout must be positive" << std::endl;
    return 1;
  }
  // Action clients
  ff_util::FreeFlyerActionClient<ff_msgs::SwitchAction> client_s_;
  ff_util::FreeFlyerActionClient<ff_msgs::MotionAction> client_t_;
  // Create a node handle
  ros::NodeHandle nh(std::string("/") + FLAGS_ns);
  // TF2 Subscriber
  tf2_ros::Buffer tf_buffer_;
  tf2_ros::TransformListener tfListener(tf_buffer_);
  // Setup SWITCH action
  client_s_.SetConnectedTimeout(FLAGS_connect);
  client_s_.SetActiveTimeout(FLAGS_active);
  client_s_.SetResponseTimeout(FLAGS_response);
  if (FLAGS_deadline > 0)
    client_s_.SetDeadlineTimeout(FLAGS_deadline);
  client_s_.SetFeedbackCallback(std::bind(
    SFeedbackCallback, std::placeholders::_1));
  client_s_.SetResultCallback(std::bind(
    SResultCallback, std::placeholders::_1, std::placeholders::_2,
    &tf_buffer_, &client_t_));
  client_s_.SetConnectedCallback(std::bind(ConnectedCallback,
    &tf_buffer_, &client_s_, &client_t_));
  client_s_.Create(&nh, ACTION_LOCALIZATION_MANAGER_SWITCH);
  // Setup MOBILITY action
  client_t_.SetConnectedTimeout(FLAGS_connect);
  client_t_.SetActiveTimeout(FLAGS_active);
  client_t_.SetResponseTimeout(FLAGS_response);
  if (FLAGS_deadline > 0)
    client_t_.SetDeadlineTimeout(FLAGS_deadline);
  client_t_.SetFeedbackCallback(std::bind(
    MFeedbackCallback, std::placeholders::_1));
  client_t_.SetResultCallback(std::bind(
    MResultCallback, std::placeholders::_1, std::placeholders::_2));
  client_t_.SetConnectedCallback(std::bind(ConnectedCallback,
    &tf_buffer_, &client_s_, &client_t_));
  client_t_.Create(&nh, ACTION_MOBILITY_MOTION);
  // For moves and executes check that we are configured correctly
  if (FLAGS_move || !FLAGS_exec.empty()) {
    ff_util::ConfigClient cfg(&nh, NODE_CHOREOGRAPHER);
    if (FLAGS_vel > 0) cfg.Set<double>("desired_vel", FLAGS_vel);
    if (FLAGS_accel > 0) cfg.Set<double>("desired_accel", FLAGS_accel);
    if (FLAGS_omega > 0) cfg.Set<double>("desired_omega", FLAGS_omega);
    if (FLAGS_alpha > 0) cfg.Set<double>("desired_alpha", FLAGS_alpha);
    if (FLAGS_rate > 0) cfg.Set<double>("desired_rate", FLAGS_rate);
    cfg.Set<bool>("enable_collision_checking", !FLAGS_nocollision);
    cfg.Set<bool>("enable_validation", !FLAGS_novalidate);
    cfg.Set<bool>("enable_bootstrapping", !FLAGS_nobootstrap);
    cfg.Set<bool>("enable_immediate", !FLAGS_noimmediate);
    cfg.Set<bool>("enable_timesync", FLAGS_timesync);
    cfg.Set<bool>("enable_replanning", FLAGS_replan);
    cfg.Set<bool>("enable_faceforward", FLAGS_ff);
    if (!FLAGS_planner.empty())
      cfg.Set<std::string>("planner", FLAGS_planner);
    if (!cfg.Reconfigure()) {
      std::cout << "Could not reconfigure the choreographer node " << std::endl;
      ros::shutdown();
    }
  }
  // Synchronous mode
  ros::spin();
  // Finish commandline flags
  google::ShutDownCommandLineFlags();
  // Make for great success
  return 0;
}

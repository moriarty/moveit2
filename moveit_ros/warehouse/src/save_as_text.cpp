/*********************************************************************
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2012, Willow Garage, Inc.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of Willow Garage nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *********************************************************************/

/* Author: Ioan Sucan */

#include <moveit/warehouse/planning_scene_storage.h>
#include <moveit/warehouse/state_storage.h>
#include <moveit/warehouse/constraints_storage.h>
#include <boost/program_options/cmdline.hpp>
#include <boost/program_options/options_description.hpp>
#include <boost/program_options/parsers.hpp>
#include <boost/program_options/variables_map.hpp>
#include <moveit/planning_scene_monitor/planning_scene_monitor.h>
#include <moveit/robot_state/conversions.h>
#include <rclcpp/executors.hpp>
#include <rclcpp/logger.hpp>
#include <rclcpp/logging.hpp>
#include <rclcpp/node.hpp>
#include <rclcpp/node_options.hpp>
#include <rclcpp/utilities.hpp>
#include <moveit/utils/logger.hpp>

static const std::string ROBOT_DESCRIPTION = "robot_description";

typedef std::pair<geometry_msgs::msg::Point, geometry_msgs::msg::Quaternion> LinkConstraintPair;
typedef std::map<std::string, LinkConstraintPair> LinkConstraintMap;

using moveit::getLogger;

void collectLinkConstraints(const moveit_msgs::msg::Constraints& constraints, LinkConstraintMap& lcmap)
{
  for (const moveit_msgs::msg::PositionConstraint& position_constraint : constraints.position_constraints)
  {
    LinkConstraintPair lcp;
    const moveit_msgs::msg::PositionConstraint& pc = position_constraint;
    lcp.first = pc.constraint_region.primitive_poses[0].position;
    lcmap[position_constraint.link_name] = lcp;
  }

  for (const moveit_msgs::msg::OrientationConstraint& orientation_constraint : constraints.orientation_constraints)
  {
    if (lcmap.count(orientation_constraint.link_name))
    {
      lcmap[orientation_constraint.link_name].second = orientation_constraint.orientation;
    }
    else
    {
      RCLCPP_WARN(getLogger(), "Orientation constraint for %s has no matching position constraint",
                  orientation_constraint.link_name.c_str());
    }
  }
}

int main(int argc, char** argv)
{
  rclcpp::init(argc, argv);
  rclcpp::NodeOptions node_options;
  node_options.allow_undeclared_parameters(true);
  node_options.automatically_declare_parameters_from_overrides(true);
  rclcpp::Node::SharedPtr node = rclcpp::Node::make_shared("save_warehouse_as_text", node_options);
  moveit::setLogger(node->get_logger());

  boost::program_options::options_description desc;
  desc.add_options()("help", "Show help message")("host", boost::program_options::value<std::string>(),
                                                  "Host for the "
                                                  "DB.")("port", boost::program_options::value<std::size_t>(),
                                                         "Port for the DB.");

  boost::program_options::variables_map vm;
  boost::program_options::store(boost::program_options::parse_command_line(argc, argv, desc), vm);
  boost::program_options::notify(vm);

  if (vm.count("help"))
  {
    std::cout << desc << '\n';
    return 1;
  }
  // Set up db
  warehouse_ros::DatabaseConnection::Ptr conn = moveit_warehouse::loadDatabase(node);
  if (vm.count("host") && vm.count("port"))
    conn->setParams(vm["host"].as<std::string>(), vm["port"].as<std::size_t>());
  if (!conn->connect())
    return 1;

  planning_scene_monitor::PlanningSceneMonitor psm(node, ROBOT_DESCRIPTION);

  moveit_warehouse::PlanningSceneStorage pss(conn);
  moveit_warehouse::RobotStateStorage rss(conn);
  moveit_warehouse::ConstraintsStorage cs(conn);

  std::vector<std::string> scene_names;
  pss.getPlanningSceneNames(scene_names);

  for (const std::string& scene_name : scene_names)
  {
    moveit_warehouse::PlanningSceneWithMetadata pswm;
    if (pss.getPlanningScene(pswm, scene_name))
    {
      RCLCPP_INFO(getLogger(), "Saving scene '%s'", scene_name.c_str());
      psm.getPlanningScene()->setPlanningSceneMsg(static_cast<const moveit_msgs::msg::PlanningScene&>(*pswm));
      std::ofstream fout((scene_name + ".scene").c_str());
      psm.getPlanningScene()->saveGeometryToStream(fout);
      fout.close();

      std::vector<std::string> robot_state_names;
      moveit::core::RobotModelConstPtr km = psm.getRobotModel();
      // Get start states for scene
      std::stringstream rsregex;
      rsregex << ".*" << scene_name << ".*";
      rss.getKnownRobotStates(rsregex.str(), robot_state_names);

      // Get goal constraints for scene
      std::vector<std::string> constraint_names;

      std::stringstream csregex;
      csregex << ".*" << scene_name << ".*";
      cs.getKnownConstraints(csregex.str(), constraint_names);

      if (!(robot_state_names.empty() && constraint_names.empty()))
      {
        std::ofstream qfout((scene_name + ".queries").c_str());
        qfout << scene_name << '\n';
        if (!robot_state_names.empty())
        {
          qfout << "start" << '\n';
          qfout << robot_state_names.size() << '\n';
          for (const std::string& robot_state_name : robot_state_names)
          {
            RCLCPP_INFO(getLogger(), "Saving start state %s for scene %s", robot_state_name.c_str(), scene_name.c_str());
            qfout << robot_state_name << '\n';
            moveit_warehouse::RobotStateWithMetadata robot_state;
            rss.getRobotState(robot_state, robot_state_name);
            moveit::core::RobotState ks(km);
            moveit::core::robotStateMsgToRobotState(*robot_state, ks, false);
            ks.printStateInfo(qfout);
            qfout << '.' << '\n';
          }
        }

        if (!constraint_names.empty())
        {
          qfout << "goal" << '\n';
          qfout << constraint_names.size() << '\n';
          for (const std::string& constraint_name : constraint_names)
          {
            RCLCPP_INFO(getLogger(), "Saving goal %s for scene %s", constraint_name.c_str(), scene_name.c_str());
            qfout << "link_constraint" << '\n';
            qfout << constraint_name << '\n';
            moveit_warehouse::ConstraintsWithMetadata constraints;
            cs.getConstraints(constraints, constraint_name);

            LinkConstraintMap lcmap;
            collectLinkConstraints(*constraints, lcmap);
            for (std::pair<const std::string, LinkConstraintPair>& iter : lcmap)
            {
              std::string link_name = iter.first;
              LinkConstraintPair lcp = iter.second;
              qfout << link_name << '\n';
              qfout << "xyz " << lcp.first.x << ' ' << lcp.first.y << ' ' << lcp.first.z << '\n';
              Eigen::Quaterniond orientation(lcp.second.w, lcp.second.x, lcp.second.y, lcp.second.z);
              Eigen::Vector3d rpy = orientation.matrix().eulerAngles(0, 1, 2);
              qfout << "rpy " << rpy[0] << ' ' << rpy[1] << ' ' << rpy[2] << '\n';
            }
            qfout << '.' << '\n';
          }
        }
        qfout.close();
      }
    }
  }

  RCLCPP_INFO(getLogger(), "Done.");
  rclcpp::spin(node);
  return 0;
}

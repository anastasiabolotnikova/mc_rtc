/*
 * Copyright 2015-2020 CNRS-UM LIRMM, CNRS-AIST JRL
 */

#include <mc_control/mc_global_controller.h>

#include <mc_control/GlobalPlugin.h>

#ifdef MC_RTC_BUILD_STATIC
#  include <mc_control/ControllerLoader.h>
#  include <mc_control/GlobalPluginLoader.h>
#endif

#include <mc_rbdyn/RobotLoader.h>

#include <mc_rtc/config.h>
#include <mc_rtc/gui/Button.h>
#include <mc_rtc/gui/Form.h>
#include <mc_rtc/gui/Label.h>
#include <mc_rtc/gui/NumberInput.h>
#include <mc_rtc/io_utils.h>
#include <mc_rtc/logging.h>

#include <RBDyn/EulerIntegration.h>
#include <RBDyn/FK.h>
#include <RBDyn/FV.h>

#include <boost/chrono.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iomanip>

namespace mc_control
{

MCGlobalController::PluginHandle::~PluginHandle() {}

MCGlobalController::MCGlobalController(const std::string & conf, std::shared_ptr<mc_rbdyn::RobotModule> rm)
: MCGlobalController(GlobalConfiguration(conf, rm))
{
}

MCGlobalController::MCGlobalController(const GlobalConfiguration & conf)
: config(conf), current_ctrl(""), next_ctrl(""), controller_(nullptr), next_controller_(nullptr)
{
  // Loading plugins
  config.load_plugin_configs();
  try
  {
    plugin_loader_.reset(new mc_rtc::ObjectLoader<mc_control::GlobalPlugin>(
        "MC_RTC_GLOBAL_PLUGIN", config.global_plugin_paths, config.use_sandbox, config.verbose_loader));
  }
  catch(mc_rtc::LoaderException & exc)
  {
    mc_rtc::log::error_and_throw<std::runtime_error>("Failed to initialize plugin loader");
  }
#ifdef MC_RTC_BUILD_STATIC
  GlobalPluginLoader::loader().enable_sandboxing(config.use_sandbox);
  GlobalPluginLoader::loader().set_verbosity(config.verbose_loader);
  auto * plugin_loader = &GlobalPluginLoader::loader();
#else
  auto * plugin_loader = plugin_loader_.get();
#endif
  for(const auto & plugin : config.global_plugins)
  {
    try
    {
      plugins_.emplace_back(plugin, plugin_loader->create_unique_object(plugin));
    }
    catch(mc_rtc::LoaderException & exc)
    {
      mc_rtc::log::error("Global plugin {} failed to load, functions provided by this plugin will not be available",
                         plugin);
    }
  }

  // Loading controller modules
  config.load_controllers_configs();
  try
  {
    controller_loader_.reset(new mc_rtc::ObjectLoader<mc_control::MCController>(
        "MC_RTC_CONTROLLER", config.controller_module_paths, config.use_sandbox, config.verbose_loader));
  }
  catch(mc_rtc::LoaderException & exc)
  {
    mc_rtc::log::error_and_throw<std::runtime_error>("Failed to initialize controller loader");
  }
#ifdef MC_RTC_BUILD_STATIC
  ControllerLoader::loader().enable_sandboxing(config.use_sandbox);
  ControllerLoader::loader().set_verbosity(config.verbose_loader);
#endif
  if(std::find(config.enabled_controllers.begin(), config.enabled_controllers.end(), "HalfSitPose")
         == config.enabled_controllers.end()
     && config.include_halfsit_controller)
  {
    config.enabled_controllers.push_back("HalfSitPose");
  }
  for(const auto & c : config.enabled_controllers)
  {
    AddController(c);
    if(c == config.initial_controller && controllers.count(c))
    {
      current_ctrl = c;
      controller_ = controllers[c].get();
    }
  }
  next_ctrl = current_ctrl;
  next_controller_ = nullptr;
  if(current_ctrl == "" || controller_ == nullptr)
  {
    mc_rtc::log::error(
        "No controller selected or selected controller is not enabled, please check your configuration file");
    mc_rtc::log::info("Note common reasons for this error include:\n"
                      "\t- The controller name does not match the name exported by CONTROLLER_CONSTRUCTOR\n"
                      "\t- The controller library is not in a path read by mc_rtc\n"
                      "\t- The controller constuctor segfaults\n"
                      "\t- The controller library hasn't been properly linked");
    mc_rtc::log::error_and_throw<std::runtime_error>("No controller enabled");
  }

  if(config.enable_log)
  {
    for(auto c : controllers)
    {
      c.second->logger().setup(config.log_policy, config.log_directory, config.log_template);
    }
  }
  if(config.enable_gui_server)
  {
    server_.reset(new mc_control::ControllerServer(config.timestep, config.gui_timestep, config.gui_server_pub_uris,
                                                   config.gui_server_rep_uris));
  }
}

MCGlobalController::~MCGlobalController() {}

std::shared_ptr<mc_rbdyn::RobotModule> MCGlobalController::get_robot_module()
{
  return config.main_robot_module;
}

std::vector<std::string> MCGlobalController::enabled_controllers() const
{
  std::vector<std::string> ret;
  for(const auto & c : controllers)
  {
    ret.push_back(c.first);
  }
  return ret;
}

std::vector<std::string> MCGlobalController::loaded_controllers() const
{
#ifndef MC_RTC_BUILD_STATIC
  return controller_loader_->objects();
#else
  return ControllerLoader::loader().objects();
#endif
}

std::vector<std::string> MCGlobalController::loaded_robots() const
{
  return mc_rbdyn::RobotLoader::available_robots();
}

std::string MCGlobalController::current_controller() const
{
  return current_ctrl;
}

void MCGlobalController::init(const std::vector<double> & initq, const std::array<double, 7> & initAttitude)
{
  Eigen::Quaterniond q{initAttitude[0], initAttitude[1], initAttitude[2], initAttitude[3]};
  Eigen::Vector3d t{initAttitude[4], initAttitude[5], initAttitude[6]};
  init(initq, sva::PTransformd(q.inverse(), t));
}

void MCGlobalController::init(const std::vector<double> & initq, const sva::PTransformd & initAttitude)
{
  initEncoders(initq);
  robot().posW(initAttitude);
  this->initController();
}

void MCGlobalController::init(const std::vector<double> & initq)
{
  initEncoders(initq);

  auto & q = robot().mbc().q;
  // Configure initial attitude (requires FK to be computed)
  if(config.init_attitude_from_sensor)
  {
    auto initAttitude = [this](const mc_rbdyn::BodySensor & sensor) {
      mc_rtc::log::info("Initializing attitude from body sensor: {}", sensor.name());
      // Update free flyer from body sensor takin into account the kinematics
      // between sensor and body
      const auto & fb = robot().mb().body(0).name();
      sva::PTransformd X_0_s(sensor.orientation(), sensor.position());
      const auto X_s_b = sensor.X_b_s().inv();
      sva::PTransformd X_b_fb = robot().X_b1_b2(sensor.parentBody(), fb);
      sva::PTransformd X_s_fb = X_b_fb * X_s_b;
      robot().posW(X_s_fb * X_0_s);
    };
    if(config.init_attitude_sensor.empty())
    {
      initAttitude(controller_->robot().bodySensor());
    }
    else
    {
      if(controller_->robot().hasBodySensor(config.init_attitude_sensor))
      {
        initAttitude(controller_->robot().bodySensor(config.init_attitude_sensor));
      }
      else
      {
        mc_rtc::log::error_and_throw<std::invalid_argument>("No body sensor named {}, could not initialize attitude. "
                                                            "Please check your InitAttitudeSensor configuration.",
                                                            config.init_attitude_sensor);
      }
    }
  }
  else
  {
    mc_rtc::log::info("Initializing attitude from robot module: q=[{}]",
                      mc_rtc::io::to_string(robot().module().default_attitude(), ", ", 3));
    if(q[0].size() == 7)
    {
      const auto & initAttitude = robot().module().default_attitude();
      q[0] = {std::begin(initAttitude), std::end(initAttitude)};
      robot().forwardKinematics();
    }
  }
  this->initController();
}

void MCGlobalController::initEncoders(const std::vector<double> & initq)
{
  if(initq.size() != controller_->robot().refJointOrder().size())
  {
    mc_rtc::log::error_and_throw<std::domain_error>(
        "Could not initialize the robot state from encoder sensors: got {} encoder values but the robot ({}) expects "
        "{}.\nMake sure that the MainRobot is compatible with the real/simulated robot used by the interface.",
        initq.size(), controller_->robot().name(), controller_->robot().refJointOrder().size());
  }

  auto & q = robot().mbc().q;
  const auto & rjo = ref_joint_order();
  for(size_t i = 0; i < rjo.size(); ++i)
  {
    const auto & jn = rjo[i];
    if(robot().hasJoint(jn))
    {
      q[robot().jointIndexByName(jn)][0] = initq[i];
    }
  }
  for(auto & g : controller_->robot().grippers())
  {
    g.get().reset(initq);
  }
  for(size_t i = 1; i < controller_->robots().size(); ++i)
  {
    auto & robot = controller_->robots().robot(i);
    if(robot.grippers().empty())
    {
      continue;
    }
    const auto & rjo = robot.refJointOrder();
    std::vector<double> rinitq;
    rinitq.reserve(rjo.size());
    for(const auto & j : rjo)
    {
      if(robot.hasJoint(j))
      {
        auto jIndex = robot.jointIndexByName(j);
        const auto & q = robot.mbc().q[jIndex];
        for(const auto & qi : q)
        {
          rinitq.push_back(qi);
        }
      }
      else
      {
        rinitq.push_back(0);
      }
    }
    for(auto & g : robot.grippers())
    {
      g.get().reset(rinitq);
    }
  }
  robot().forwardKinematics();
}

void MCGlobalController::initController()
{
  if(config.enable_log)
  {
    start_log();
  }
  const auto & q = robot().mbc().q;
  controller_->reset({q});
  controller_->resetObserverPipelines();
  initGUI();
  for(auto & plugin : plugins_)
  {
    plugin.plugin->init(*this, config.global_plugin_configs[plugin.name]);
  }
}

void MCGlobalController::setSensorPosition(const Eigen::Vector3d & pos)
{
  robot().bodySensor().position(pos);
  realRobot().bodySensor().position(pos);
}

void MCGlobalController::setSensorPosition(const std::string & robotName, const Eigen::Vector3d & pos)
{
  robot(robotName).bodySensor().position(pos);
  realRobot(robotName).bodySensor().position(pos);
}

void MCGlobalController::setSensorPositions(const std::map<std::string, Eigen::Vector3d> & poses)
{
  setSensorPositions(robot(), poses);
  setSensorPositions(realRobot(), poses);
}

void MCGlobalController::setSensorPositions(const std::string & robotName,
                                            const std::map<std::string, Eigen::Vector3d> & poses)
{
  setSensorPositions(robot(robotName), poses);
  setSensorPositions(realRobot(robotName), poses);
}

void MCGlobalController::setSensorPositions(mc_rbdyn::Robot & robot,
                                            const std::map<std::string, Eigen::Vector3d> & poses)
{
  for(const auto & p : poses)
  {
    robot.bodySensor(p.first).position(p.second);
  }
}

void MCGlobalController::setSensorOrientation(const Eigen::Quaterniond & ori)
{
  robot().bodySensor().orientation(ori);
  realRobot().bodySensor().orientation(ori);
}

void MCGlobalController::setSensorOrientation(const std::string & robotName, const Eigen::Quaterniond & ori)
{
  robot(robotName).bodySensor().orientation(ori);
  realRobot(robotName).bodySensor().orientation(ori);
}

void MCGlobalController::setSensorOrientations(const QuaternionMap & oris)
{
  setSensorOrientations(robot(), oris);
  setSensorOrientations(realRobot(), oris);
}

void MCGlobalController::setSensorOrientations(const std::string & robotName, const QuaternionMap & oris)
{
  setSensorOrientations(robot(robotName), oris);
  setSensorOrientations(realRobot(robotName), oris);
}

void MCGlobalController::setSensorOrientations(mc_rbdyn::Robot & robot, const QuaternionMap & oris)
{
  for(const auto & o : oris)
  {
    robot.bodySensor(o.first).orientation(o.second);
  }
}

void MCGlobalController::setSensorLinearVelocity(const Eigen::Vector3d & vel)
{
  robot().bodySensor().linearVelocity(vel);
  realRobot().bodySensor().linearVelocity(vel);
}

void MCGlobalController::setSensorLinearVelocity(const std::string & robotName, const Eigen::Vector3d & vel)
{
  robot(robotName).bodySensor().linearVelocity(vel);
  realRobot(robotName).bodySensor().linearVelocity(vel);
}

void MCGlobalController::setSensorLinearVelocities(const std::map<std::string, Eigen::Vector3d> & linearVels)
{
  setSensorLinearVelocities(robot(), linearVels);
  setSensorLinearVelocities(realRobot(), linearVels);
}

void MCGlobalController::setSensorLinearVelocities(const std::string & robotName,
                                                   const std::map<std::string, Eigen::Vector3d> & linearVels)
{
  setSensorLinearVelocities(robot(robotName), linearVels);
  setSensorLinearVelocities(realRobot(robotName), linearVels);
}

void MCGlobalController::setSensorLinearVelocities(mc_rbdyn::Robot & robot,
                                                   const std::map<std::string, Eigen::Vector3d> & linearVels)
{
  for(const auto & lv : linearVels)
  {
    robot.bodySensor(lv.first).linearVelocity(lv.second);
  }
}

void MCGlobalController::setSensorAngularVelocity(const Eigen::Vector3d & vel)
{
  robot().bodySensor().angularVelocity(vel);
  realRobot().bodySensor().angularVelocity(vel);
}

void MCGlobalController::setSensorAngularVelocity(const std::string & name, const Eigen::Vector3d & vel)
{
  robot(name).bodySensor().angularVelocity(vel);
  realRobot(name).bodySensor().angularVelocity(vel);
}

void MCGlobalController::setSensorAngularVelocities(const std::map<std::string, Eigen::Vector3d> & angularVels)
{
  setSensorAngularVelocities(robot(), angularVels);
  setSensorAngularVelocities(realRobot(), angularVels);
}

void MCGlobalController::setSensorAngularVelocities(const std::string & name,
                                                    const std::map<std::string, Eigen::Vector3d> & angularVels)
{
  setSensorAngularVelocities(robot(name), angularVels);
  setSensorAngularVelocities(realRobot(name), angularVels);
}

void MCGlobalController::setSensorAngularVelocities(mc_rbdyn::Robot & robot,
                                                    const std::map<std::string, Eigen::Vector3d> & angularVels)
{
  for(const auto & av : angularVels)
  {
    robot.bodySensor(av.first).angularVelocity(av.second);
  }
}

void MCGlobalController::setSensorAcceleration(const Eigen::Vector3d & acc)
{
  setSensorLinearAcceleration(acc);
}

void MCGlobalController::setSensorAccelerations(const std::map<std::string, Eigen::Vector3d> & accels)
{
  setSensorLinearAccelerations(accels);
}

void MCGlobalController::setSensorAccelerations(mc_rbdyn::Robot & robot,
                                                const std::map<std::string, Eigen::Vector3d> & accels)
{
  setSensorLinearAccelerations(robot, accels);
}

void MCGlobalController::setSensorLinearAcceleration(const Eigen::Vector3d & acc)
{
  robot().bodySensor().linearAcceleration(acc);
  realRobot().bodySensor().linearAcceleration(acc);
}

void MCGlobalController::setSensorLinearAcceleration(const std::string & name, const Eigen::Vector3d & acc)
{
  robot(name).bodySensor().linearAcceleration(acc);
  realRobot(name).bodySensor().linearAcceleration(acc);
}

void MCGlobalController::setSensorLinearAccelerations(const std::map<std::string, Eigen::Vector3d> & accels)
{
  setSensorLinearAccelerations(robot(), accels);
  setSensorLinearAccelerations(realRobot(), accels);
}

void MCGlobalController::setSensorLinearAccelerations(const std::string & name,
                                                      const std::map<std::string, Eigen::Vector3d> & accels)
{
  setSensorLinearAccelerations(robot(name), accels);
  setSensorLinearAccelerations(realRobot(name), accels);
}

void MCGlobalController::setSensorLinearAccelerations(mc_rbdyn::Robot & robot,
                                                      const std::map<std::string, Eigen::Vector3d> & accels)
{
  for(const auto & a : accels)
  {
    robot.bodySensor(a.first).linearAcceleration(a.second);
  }
}

void MCGlobalController::setSensorAngularAcceleration(const Eigen::Vector3d & acc)
{
  robot().bodySensor().angularAcceleration(acc);
  realRobot().bodySensor().angularAcceleration(acc);
}

void MCGlobalController::setSensorAngularAccelerations(const std::map<std::string, Eigen::Vector3d> & accels)
{
  setSensorAngularAccelerations(robot(), accels);
  setSensorAngularAccelerations(realRobot(), accels);
}

void MCGlobalController::setSensorAngularAccelerations(mc_rbdyn::Robot & robot,
                                                       const std::map<std::string, Eigen::Vector3d> & accels)
{
  for(const auto & a : accels)
  {
    robot.bodySensor(a.first).angularAcceleration(a.second);
  }
}

void MCGlobalController::setEncoderValues(const std::vector<double> & eValues)
{
  robot().encoderValues(eValues);
  realRobot().encoderValues(eValues);
}

void MCGlobalController::setEncoderValues(const std::string & robotName, const std::vector<double> & eValues)
{
  robot(robotName).encoderValues(eValues);
  realRobot(robotName).encoderValues(eValues);
}

void MCGlobalController::setEncoderVelocities(const std::vector<double> & eVelocities)
{
  robot().encoderVelocities(eVelocities);
  realRobot().encoderVelocities(eVelocities);
}

void MCGlobalController::setEncoderVelocities(const std::string & robotName, const std::vector<double> & eVelocities)
{
  robot(robotName).encoderVelocities(eVelocities);
  realRobot(robotName).encoderVelocities(eVelocities);
}

void MCGlobalController::setFlexibilityValues(const std::vector<double> & fValues)
{
  robot().flexibilityValues(fValues);
  realRobot().flexibilityValues(fValues);
}

void MCGlobalController::setFlexibilityValues(const std::string & robotName, const std::vector<double> & fValues)
{
  robot(robotName).flexibilityValues(fValues);
  realRobot(robotName).flexibilityValues(fValues);
}

void MCGlobalController::setJointTorques(const std::vector<double> & tValues)
{
  robot().jointTorques(tValues);
  realRobot().jointTorques(tValues);
}

void MCGlobalController::setJointTorques(const std::string & robotName, const std::vector<double> & tValues)
{
  robot(robotName).jointTorques(tValues);
  realRobot(robotName).jointTorques(tValues);
}

void MCGlobalController::setWrenches(const std::map<std::string, sva::ForceVecd> & wrenches)
{
  setWrenches(controller_->robot().name(), wrenches);
}

void MCGlobalController::setWrenches(const std::string & robotName,
                                     const std::map<std::string, sva::ForceVecd> & wrenches)
{
  auto & robot = this->robot(robotName);
  auto & realRobot = this->realRobot(robotName);
  for(const auto & w : wrenches)
  {
    robot.forceSensor(w.first).wrench(w.second);
    realRobot.forceSensor(w.first).wrench(w.second);
  }
}

// deprecated
void MCGlobalController::setWrenches(unsigned int robotIndex, const std::map<std::string, sva::ForceVecd> & wrenches)
{
  auto & robot = controller_->robots().robot(robotIndex);
  auto & realRobot = controller_->realRobots().robot(robotIndex);
  for(const auto & w : wrenches)
  {
    robot.forceSensor(w.first).wrench(w.second);
    realRobot.forceSensor(w.first).wrench(w.second);
  }
}

bool MCGlobalController::run()
{
  /** Always pick a steady clock */
  using clock = typename std::conditional<std::chrono::high_resolution_clock::is_steady,
                                          std::chrono::high_resolution_clock, std::chrono::steady_clock>::type;
  /** Helper to converst Tasks' timer */
  using boost_ms = boost::chrono::duration<double, boost::milli>;
  using boost_ns = boost::chrono::duration<double, boost::nano>;
  auto start_run_t = clock::now();
  /* Check if we need to change the controller this time */
  if(next_controller_)
  {
    mc_rtc::log::info("Switching controllers");
    if(controller_)
    {
      for(auto & bs : next_controller_->robot().bodySensors())
      {
        const auto & current = controller_->robot().bodySensor(bs.name());
        bs.position(current.position());
        bs.orientation(current.orientation());
        bs.linearVelocity(current.linearVelocity());
        bs.angularVelocity(current.angularVelocity());
        bs.linearAcceleration(current.linearAcceleration());
        bs.angularAcceleration(current.angularAcceleration());
      }
      next_controller_->robot().encoderValues(controller_->robot().encoderValues());
      next_controller_->realRobot().encoderValues(controller_->robot().encoderValues());
      next_controller_->robot().encoderVelocities(controller_->robot().encoderVelocities());
      next_controller_->realRobot().encoderVelocities(controller_->robot().encoderVelocities());
      next_controller_->robot().jointTorques(controller_->robot().jointTorques());
      next_controller_->realRobot().jointTorques(controller_->robot().jointTorques());
      for(auto & fs : next_controller_->robot().forceSensors())
      {
        fs.wrench(controller_->robot().forceSensor(fs.name()).wrench());
      }
      for(auto & fs : next_controller_->realRobot().forceSensors())
      {
        fs.wrench(controller_->realRobot().forceSensor(fs.name()).wrench());
      }
    }
    if(!running)
    {
      controller_ = next_controller_;
    }
    else
    {
      // Remove observer pipelines created by MCController::createObserverPipelines
      for(auto & pipeline : controller_->observerPipelines())
      {
        if(controller_->gui_)
        {
          pipeline.removeFromGUI(*controller_->gui());
        }
        pipeline.removeFromLogger(controller_->logger());
      }
      controller_->stop();
      mc_rtc::log::info("Reset with q[0] = {}", mc_rtc::io::to_string(controller_->robot().mbc().q[0], ", ", 5));
      for(const auto & g : controller_->robot().grippersByName())
      {
        next_controller_->robot().gripper(g.first).reset(*g.second);
      }
      next_controller_->reset({controller_->robot().mbc().q});
      next_controller_->resetObserverPipelines();
      controller_ = next_controller_;
      /** Reset plugins */
      for(auto & plugin : plugins_)
      {
        plugin.plugin->reset(*this);
      }
    }
    next_controller_ = 0;
    current_ctrl = next_ctrl;
    if(config.enable_log)
    {
      start_log();
    }
    initGUI();
    mc_rtc::log::success("Controller {} activated", current_ctrl);
  }
  for(auto & plugin : plugins_)
  {
    auto start_t = clock::now();
    plugin.plugin->before(*this);
    plugin.plugin_before_dt = clock::now() - start_t;
  }
  if(running)
  {
    for(size_t i = 0; i < pre_gripper_mbcs_.size() && i < controller_->robots().size(); ++i)
    {
      controller_->robots().robot(i).mbc() = pre_gripper_mbcs_[i];
    }
    auto start_observers_run_t = clock::now();
    controller_->runObserverPipelines();
    observers_run_dt = clock::now() - start_observers_run_t;

    auto start_controller_run_t = clock::now();
    bool r = controller_->run();
    auto end_controller_run_t = clock::now();

    pre_gripper_mbcs_ = controller_->robots().mbcs();
    for(size_t i = 0; i < controller_->robots().size(); ++i)
    {
      const auto & gi = controller_->robots().robot(i).grippers();
      if(gi.empty())
      {
        continue;
      }
      for(auto & g : gi)
      {
        g.get().run(controller_->timeStep, controller_->robots().robot(i),
                    controller_->solver().result().robots_state[i].q);
      }
      controller_->robots().robot(i).forwardKinematics();
    }
    if(config.enable_log)
    {
      auto start_log_t = clock::now();
      controller_->logger().log();
      log_dt = clock::now() - start_log_t;
    }
    if(server_)
    {
      auto start_gui_t = clock::now();
      server_->handle_requests(*controller_->gui_);
      server_->publish(*controller_->gui_);
      gui_dt = clock::now() - start_gui_t;
    }
    controller_run_dt = end_controller_run_t - start_controller_run_t;
    solver_build_and_solve_t = boost_ms(boost_ns(controller_->solver().solveAndBuildTime().wall)).count();
    solver_solve_t = boost_ms(boost_ns(controller_->solver().solveTime().wall)).count();
    if(!r)
    {
      running = false;
    }
  }
  else
  {
    controller_run_dt.zero();
    solver_build_and_solve_t = 0;
    solver_solve_t = 0;
    if(server_)
    {
      auto start_gui_t = clock::now();
      server_->handle_requests(*controller_->gui_);
      server_->publish(*controller_->gui_);
      gui_dt = clock::now() - start_gui_t;
    }
  }
  for(auto & plugin : plugins_)
  {
    auto start_t = clock::now();
    plugin.plugin->after(*this);
    plugin.plugin_after_dt = clock::now() - start_t;
  }
  global_run_dt = clock::now() - start_run_t;
  // Percentage of time not spent inside the user code
  framework_cost = 100 * (1 - controller_run_dt.count() / global_run_dt.count());
  return running;
}

ControllerServer & MCGlobalController::server()
{
  assert(server_);
  return *server_;
}

const mc_solver::QPResultMsg & MCGlobalController::send(const double & t)
{
  return controller_->send(t);
}

MCController & MCGlobalController::controller()
{
  assert(controller_ != nullptr);
  return *controller_;
}

const MCController & MCGlobalController::controller() const
{
  assert(controller_ != nullptr);
  return *controller_;
}

mc_rbdyn::Robots & MCGlobalController::realRobots()
{
  return controller_->realRobots();
}

const mc_rbdyn::Robots & MCGlobalController::realRobots() const
{
  return controller_->realRobots();
}

mc_rbdyn::Robots & MCGlobalController::robots()
{
  return controller_->robots();
}

const mc_rbdyn::Robots & MCGlobalController::robots() const
{
  return controller_->robots();
}

mc_rbdyn::Robot & MCGlobalController::robot()
{
  return controller_->robot();
}

const mc_rbdyn::Robot & MCGlobalController::robot() const
{
  return controller_->robot();
}

mc_rbdyn::Robot & MCGlobalController::robot(const std::string & name)
{
  return robots().robot(name);
}

const mc_rbdyn::Robot & MCGlobalController::robot(const std::string & name) const
{
  return robots().robot(name);
}

mc_rbdyn::Robot & MCGlobalController::realRobot()
{
  return controller_->realRobot();
}

const mc_rbdyn::Robot & MCGlobalController::realRobot() const
{
  return controller_->realRobot();
}

mc_rbdyn::Robot & MCGlobalController::realRobot(const std::string & name)
{
  return realRobots().robot(name);
}

const mc_rbdyn::Robot & MCGlobalController::realRobot(const std::string & name) const
{
  return realRobots().robot(name);
}

void MCGlobalController::setGripperTargetQ(const std::string & robot,
                                           const std::string & name,
                                           const std::vector<double> & q)
{
  try
  {
    auto & gripper = controller_->gripper(robot, name);
    gripper.setTargetQ(q);
  }
  catch(const std::exception &)
  {
    mc_rtc::log::error("Cannot set gripper opening for non-existing gripper {} in {}", name, robot);
  }
}

void MCGlobalController::setGripperOpenPercent(const std::string & robot, double pOpen)
{
  auto & r = controller_->robots().robot(robot);
  for(auto & g : r.grippers())
  {
    g.get().setTargetOpening(pOpen);
  }
}

void MCGlobalController::setGripperOpenPercent(const std::string & robot, const std::string & name, double pOpen)
{
  try
  {
    auto & gripper = controller_->gripper(robot, name);
    gripper.setTargetOpening(pOpen);
  }
  catch(const std::exception &)
  {
    mc_rtc::log::error("Cannot set gripper opening for non-existing gripper {} in {}", name, robot);
  }
}

double MCGlobalController::timestep() const
{
  return config.timestep;
}

const std::vector<std::string> & MCGlobalController::ref_joint_order()
{
  return robot().refJointOrder();
}

bool MCGlobalController::AddController(const std::string & name)
{
  if(controllers.count(name))
  {
    mc_rtc::log::warning("Controller {} already enabled", name);
    return false;
  }
  std::string controller_name = name;
  std::string controller_subname = "";
  size_t sep_pos = name.find('#');
  if(sep_pos != std::string::npos)
  {
    controller_name = name.substr(0, sep_pos);
    controller_subname = name.substr(sep_pos + 1);
  }
#ifndef MC_RTC_BUILD_STATIC
  auto * controller_loader = controller_loader_.get();
#else
  auto * controller_loader = &ControllerLoader::loader();
#endif
  if(controller_loader->has_object(controller_name))
  {
    mc_rtc::log::info("Create controller {}", controller_name);
    if(controller_subname != "")
    {
      controllers[name] =
          controller_loader->create_object(controller_name, controller_subname, config.main_robot_module,
                                           config.timestep, config.controllers_configs[name]);
    }
    else
    {
      controllers[name] = controller_loader->create_object(name, config.main_robot_module, config.timestep,
                                                           config.controllers_configs[name]);
    }
    controllers[name]->name_ = name;
    if(config.enable_log)
    {
      controllers[name]->logger().setup(config.log_policy, config.log_directory, config.log_template);
    }
    controllers[name]->createObserverPipelines(config.controllers_configs[name]);
    return true;
  }
  else
  {
    mc_rtc::log::warning("Controller {} enabled in configuration but not available", name);
    return false;
  }
}

const MCGlobalController::GlobalConfiguration & MCGlobalController::configuration() const
{
  return config;
}

void MCGlobalController::add_controller_module_paths(const std::vector<std::string> & paths)
{
  controller_loader_->load_libraries(paths);
}

bool MCGlobalController::AddController(const std::string & name, std::shared_ptr<mc_control::MCController> controller)
{
  if(controllers.count(name) || !controller)
  {
    mc_rtc::log::warning("Controller {} already enabled or invalid pointer passed", name);
    return false;
  }
  controllers[name] = controller;
  if(config.enable_log)
  {
    controllers[name]->logger().setup(config.log_policy, config.log_directory, config.log_template);
  }
  return true;
}

bool MCGlobalController::EnableController(const std::string & name)
{
  if(name != current_ctrl && controllers.count(name))
  {
    next_ctrl = name;
    next_controller_ = controllers[name].get();
    return true;
  }
  else
  {
    if(name == current_ctrl)
    {
      mc_rtc::log::error("{} controller already enabled.", name);
    }
    else
    {
      mc_rtc::log::error("{} controller not enabled.", name);
    }
    return false;
  }
}

bool MCGlobalController::GoToHalfSitPose()
{
  if(current_ctrl != std::string("HalfSitPose"))
  {
    return EnableController("HalfSitPose");
  }
  return true;
}

void MCGlobalController::start_log()
{
  controller_->logger().start(current_ctrl, controller_->timeStep);
  setup_log();
}

void MCGlobalController::refreshLog()
{
  controller_->logger().start(current_ctrl, controller_->timeStep, true);
  setup_log();
}

void MCGlobalController::setup_log()
{
  if(setup_logger_.count(current_ctrl))
  {
    return;
  }
  // Copy controller pointer to avoid lambda issue
  MCController * controller = controller_;
  controller->logger().addLogEntry(
      "qIn", [controller]() -> const std::vector<double> & { return controller->robot().encoderValues(); });
  controller->logger().addLogEntry(
      "ff", [controller]() -> const sva::PTransformd & { return controller->robot().mbc().bodyPosW[0]; });
  // Convert reference order to mbc.q, -1 if the joint does not exist
  std::vector<int> refToQ;
  for(const auto & j : controller->robot().refJointOrder())
  {
    if(controller->robot().hasJoint(j))
    {
      int jIndex = static_cast<int>(controller->robot().jointIndexByName(j));
      if(controller->robot().mb().joint(jIndex).dof() == 1)
      {
        refToQ.push_back(jIndex);
        continue;
      }
    }
    refToQ.push_back(-1);
  }
  std::vector<double> qOut(controller->robot().refJointOrder().size(), 0);
  controller->logger().addLogEntry("qOut", [controller, refToQ, qOut]() mutable -> const std::vector<double> & {
    for(size_t i = 0; i < qOut.size(); ++i)
    {
      if(refToQ[i] != -1)
      {
        qOut[i] = controller->robot().mbc().q[static_cast<size_t>(refToQ[i])][0];
      }
    }
    return qOut;
  });
  controller->logger().addLogEntry(
      "tauIn", [controller]() -> const std::vector<double> & { return controller->robot().jointTorques(); });
  for(const auto & fs : controller->robot().forceSensors())
  {
    const auto & fs_name = fs.name();
    controller->logger().addLogEntry(fs.name(), [controller, fs_name]() -> const sva::ForceVecd & {
      return controller->robot().forceSensor(fs_name).wrench();
    });
  }

  // Log all body sensors
  const auto & bodySensors = controller->robot().bodySensors();
  for(size_t i = 0; i < bodySensors.size(); ++i)
  {
    const auto & name = bodySensors[i].name();
    controller->logger().addLogEntry(name + "_position", [controller, name]() -> const Eigen::Vector3d & {
      return controller->robot().bodySensor(name).position();
    });
    controller->logger().addLogEntry(name + "_orientation", [controller, name]() -> const Eigen::Quaterniond & {
      return controller->robot().bodySensor(name).orientation();
    });
    controller->logger().addLogEntry(name + "_linearVelocity", [controller, name]() -> const Eigen::Vector3d & {
      return controller->robot().bodySensor(name).linearVelocity();
    });
    controller->logger().addLogEntry(name + "_angularVelocity", [controller, name]() -> const Eigen::Vector3d & {
      return controller->robot().bodySensor(name).angularVelocity();
    });
    controller->logger().addLogEntry(name + "_linearAcceleration", [controller, name]() -> const Eigen::Vector3d & {
      return controller->robot().bodySensor(name).linearAcceleration();
    });
    controller->logger().addLogEntry(name + "_angularAcceleration", [controller, name]() -> const Eigen::Vector3d & {
      return controller->robot().bodySensor(name).angularAcceleration();
    });
  }

  if(config.log_real)
  {
    controller->logger().addLogEntry("realRobot_ff", [controller]() -> const sva::PTransformd & {
      return controller->realRobots().robot().mbc().bodyPosW[0];
    });
    controller->logger().addLogEntry(
        "realRobot_q", [controller]() -> const std::vector<double> & { return controller->robot().encoderValues(); });
    controller->logger().addLogEntry("realRobot_alpha", [controller]() -> const std::vector<double> & {
      return controller->robot().encoderVelocities();
    });
  }

  // Performance measures
  controller->logger().addLogEntry("perf_GlobalRun", [this]() { return global_run_dt.count(); });
  controller->logger().addLogEntry("perf_ControllerRun", [this]() { return controller_run_dt.count(); });
  controller->logger().addLogEntry("perf_ObserversRun", [this]() { return observers_run_dt.count(); });
  controller->logger().addLogEntry("perf_SolverBuildAndSolve", [this]() { return solver_build_and_solve_t; });
  controller->logger().addLogEntry("perf_SolverSolve", [this]() { return solver_solve_t; });
  controller->logger().addLogEntry("perf_Log", [this]() { return log_dt.count(); });
  controller->logger().addLogEntry("perf_Gui", [this]() { return gui_dt.count(); });
  controller->logger().addLogEntry("perf_FrameworkCost", [this]() { return framework_cost; });
  for(const auto & plugin : plugins_)
  {
    controller->logger().addLogEntry("perf_Plugins_" + plugin.name, [&plugin]() {
      return plugin.plugin_before_dt.count() + plugin.plugin_after_dt.count();
    });
    controller->logger().addLogEntry("perf_Plugins_" + plugin.name + "_before",
                                     [&plugin]() { return plugin.plugin_before_dt.count(); });
    controller->logger().addLogEntry("perf_Plugins_" + plugin.name + "_after",
                                     [&plugin]() { return plugin.plugin_after_dt.count(); });
  }
  // Log system wall time as nanoseconds since epoch (can be used to manage synchronization with ros)
  controller->logger().addLogEntry("timeWall", []() -> int64_t {
    int64_t nanoseconds_since_epoch = std::chrono::system_clock::now().time_since_epoch() / std::chrono::nanoseconds(1);
    return nanoseconds_since_epoch;
  });
  setup_logger_[current_ctrl] = true;
}

} // namespace mc_control

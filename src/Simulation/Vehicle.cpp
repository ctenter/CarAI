
#include <glad/glad.h>

#include "Vehicle.h"
#include "Simulation.h"

#include "../Application.h"

#include <iostream>
#include <algorithm>
#include <cstdio>

Vehicle::Vehicle(btRaycastVehicle* bvehicle)
  : m_vehicle(bvehicle), m_controller(0), m_neuralNetwork(0),
  m_bestSegment(0), m_curSegment(0), m_travelDir(0), m_bestDistance(0.0f), m_curDistance(0.0f),
  m_alive(true)
{

}

Vehicle::~Vehicle()
{
  delete m_vehicle;
  delete m_controller;
  delete m_neuralNetwork;
}




void Vehicle::setControllerRand()
{
  delete m_controller;
  m_controller = new VehicleControllerRand(this);
}


void Vehicle::setControllerUser(Application* app)
{
  delete m_controller;
  m_controller = new VehicleControllerUser(this, app);
}


void Vehicle::setControllerNeuralNet()
{
  delete m_controller;
  m_controller = new VehicleControllerNeuralNet(this);
}

void Vehicle::update(double dt, Simulation* sim)
{
  // update sensors
  for (int i = 0; i < numSensors(); ++i)
  {
    Sensor* s = sensor(i);

    s->startWS = m_vehicle->getChassisWorldTransform() * s->startOS;
    s->endWS = m_vehicle->getChassisWorldTransform() * s->endOS;

    btCollisionWorld::ClosestRayResultCallback hit(s->startWS, s->endWS);

    // ignore other vehicles in the simulation
    hit.m_collisionFilterGroup = collisionGroup();
    hit.m_collisionFilterMask = ~collisionGroup();

    sim->world()->rayTest(hit.m_rayFromWorld, hit.m_rayToWorld, hit);
    
    s->dist = s->maxDist;
    if (hit.hasHit())
      s->dist *= hit.m_closestHitFraction;
  }

  // update performance
  updateTrackPerformance(sim);


  if (m_controller)
    m_controller->update(dt);
}

void Vehicle::addSensor(const btVector3& start, const btVector3& end)
{
  Sensor s;
  s.startOS = start;
  s.endOS = end;

  s.maxDist = (start - end).norm();
  s.dist = s.maxDist;

  s.startWS = m_vehicle->getChassisWorldTransform() * s.startOS;
  s.endWS = m_vehicle->getChassisWorldTransform() * s.endOS;

  m_sensors.push_back(s);
}


void Vehicle::initNeuralNetwork(const std::vector<int>& internalLayerSize)
{
  delete m_neuralNetwork;
  m_neuralNetwork = new NeuralNetwork();


  // sensor layer
  m_neuralNetwork->addLayer(numSensors());

  // internal layers
  for (size_t i = 0; i < internalLayerSize.size(); ++i)
    m_neuralNetwork->addLayer(internalLayerSize[i]);

  // output layer
  m_neuralNetwork->addLayer(VehicleControllerNeuralNet::dof());


  // init with random weights
  for (int i = 0; i < m_neuralNetwork->numLinks(); ++i)
    m_neuralNetwork->links(i)->randomize(-1.0f, 1.0f);
}


void Vehicle::updateTrackPerformance(Simulation* sim)
{
  const std::vector<btVector3>& segments = sim->trackSegments();
  const std::vector<float>& distances = sim->trackSegmentDist();


  // brute force: find nearest segment
  int nearestSeg = -1;
  float nearestSegDist = -1.0f;
  btVector3 nearestSegProj;
  float trackDist = -1.0f;

  btVector3 vpos = m_vehicle->getChassisWorldTransform().getOrigin();

  int nsegs = static_cast<int>(sim->trackSegments().size());
  for (int i = 0; i < nsegs; ++i)
  {
    // project vehicle pos onto segment
    btVector3 a = segments[i];
    btVector3 n = segments[(i + 1) % nsegs] - a;
    btScalar len = n.norm();
    n /= len;

    btScalar p = (vpos - a).dot(n);

    if (0.0f <= p && p <= len)
    {
      btVector3 segProj = a + n * p;
      float segDist = (segProj - vpos).norm();

      // segment found
      if (nearestSeg < 0 || segDist < nearestSegDist)
      {
        nearestSeg = i;
        nearestSegProj = segProj;
        nearestSegDist = segDist;

        trackDist = distances[i] + p;
      }
    }
  }

  if (nearestSeg >= 0)
  {
    // update travel direction
    const btVector3& vel = m_vehicle->getRigidBody()->getLinearVelocity();
    btVector3 n = segments[(nearestSeg + 1) % nsegs] - segments[nearestSeg];

    btScalar vdotn = vel.dot(n);

    if (vel.dot(vel) < 1e-6f)
      m_travelDir = 0;
    else if (vdotn > 0)
      m_travelDir = 1;
    else if (vdotn < 0)
      m_travelDir = -1;

    // update segment info
    m_bestSegment = std::max(m_bestSegment, nearestSeg);
    m_bestDistance = std::max(m_bestDistance, trackDist);

    m_curSegment = nearestSeg;
    m_curDistance = trackDist;
  }
}



VehicleController::VehicleController(Vehicle* vehicle)
  : m_vehicle(vehicle),
  m_steerMax(0.6f),
  m_engineForceFwdMax(5000.0f), m_engineForceRevMax(-3000.0f),
  m_brakeMax(500.0f)
{

}

VehicleControllerUser::VehicleControllerUser(Vehicle* vehicle, Application* app)
  : VehicleController(vehicle)
{
  app->addUserInputController(this);
}

void VehicleControllerUser::keyEvent(GLFWwindow* wnd, int key, int scancode, int action, int mods)
{
  if (m_vehicle)
  {
    if (action != GLFW_RELEASE)
    {
      if (key == GLFW_KEY_LEFT)
      {
        m_vehicle->physics()->setSteeringValue(m_steerMax, 0);
        m_vehicle->physics()->setSteeringValue(m_steerMax, 1);
      }

      if (key == GLFW_KEY_RIGHT)
      {
        m_vehicle->physics()->setSteeringValue(-m_steerMax, 0);
        m_vehicle->physics()->setSteeringValue(-m_steerMax, 1);
      }

      if (key == GLFW_KEY_UP)
      {
        m_vehicle->physics()->applyEngineForce(m_engineForceFwdMax, 2);
        m_vehicle->physics()->applyEngineForce(m_engineForceFwdMax, 3);
      }

      if (key == GLFW_KEY_DOWN)
      {
        m_vehicle->physics()->applyEngineForce(m_engineForceRevMax, 2);
        m_vehicle->physics()->applyEngineForce(m_engineForceRevMax, 3);
      }

      //Handbrake
      if (key == GLFW_KEY_SPACE)
      {
        m_vehicle->physics()->setBrake(m_brakeMax, 2);
        m_vehicle->physics()->setBrake(m_brakeMax, 3);
      }
    }
    else
    {
      if (key == GLFW_KEY_LEFT || key == GLFW_KEY_RIGHT)
      {
        m_vehicle->physics()->setSteeringValue(0, 0);
        m_vehicle->physics()->setSteeringValue(0, 1);
      }

      if (key == GLFW_KEY_UP || key == GLFW_KEY_DOWN)
      {
        m_vehicle->physics()->applyEngineForce(0, 2);
        m_vehicle->physics()->applyEngineForce(0, 3);

        //Default braking force, always added otherwise there is no friction on the wheels
        m_vehicle->physics()->setBrake(10, 2);
        m_vehicle->physics()->setBrake(10, 3);
      }

      //Handbrake
      if (key == GLFW_KEY_SPACE)
      {
        m_vehicle->physics()->setBrake(0, 2);
        m_vehicle->physics()->setBrake(0, 3);
      }
    }
  }
}

VehicleControllerRand::VehicleControllerRand(Vehicle* vehicle) : VehicleController(vehicle)
{

}

void VehicleControllerRand::update(double dt)
{
  float u = static_cast<float>(std::rand()) / RAND_MAX;

  m_vehicle->physics()->setSteeringValue((-1.0f + 2.0f * u) * m_steerMax, 0);
  m_vehicle->physics()->setSteeringValue((-1.0f + 2.0f * u) * m_steerMax, 1);

  u = static_cast<float>(std::rand()) / RAND_MAX;
  m_vehicle->physics()->applyEngineForce(u * m_engineForceFwdMax, 2);
  m_vehicle->physics()->applyEngineForce(u * m_engineForceFwdMax, 3);
}


VehicleControllerNeuralNet::VehicleControllerNeuralNet(Vehicle* vehicle)
  : VehicleController(vehicle)
{

}

void VehicleControllerNeuralNet::update(double dt)
{
  // get sensor data
  int ns = m_vehicle->numSensors();
  std::vector<float> input(ns);

  for (int i = 0; i < ns; ++i)
    input[i] = m_vehicle->sensor(i)->dist;

  // get output from neural network
  std::vector<float> output;
  if (m_vehicle->neuralNetwork()->compute(input, output))
  {
    // apply to vehicle
    btRaycastVehicle* v = m_vehicle->physics();


    // clamp to allowed values
    float steer = output[0] * m_steerMax;
    
    float force = output[1] * 0.5f + 0.5f; // map [-1,1] to [0,1]
    force = m_engineForceRevMax + (m_engineForceFwdMax - m_engineForceRevMax) * force; // lerp

    v->setSteeringValue(steer, 0);
    v->setSteeringValue(steer, 1);

    v->applyEngineForce(force, 2);
    v->applyEngineForce(force, 3);
  }
  else
    std::cerr << "error: could not execute neural network" << std::endl;
}

int VehicleControllerNeuralNet::dof()
{
  // dofs: steer, engine force
  return 2;
}
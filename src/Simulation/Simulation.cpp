
#include <glad/glad.h>

#include "Simulation.h"


#include <BulletCollision/CollisionShapes/btHeightfieldTerrainShape.h>


#include <lodepng.h>
#include <iostream>
#include <fstream>
#include <algorithm>

Simulation::Simulation(const Simulation::Desc& desc, Application* app)
  : m_desc(desc), m_app(app), m_bullet(0), m_groundBody(0), m_sphereBody(0), m_vehicleUser(0),
  m_evolution(0), m_trackBody(0)
{
  m_bullet = new BulletInterface();
  m_bullet->world->setGravity(btVector3(0, -10, 0));

  //m_groundBody = m_bullet->createManagedRigidBody(std::make_shared<btStaticPlaneShape>(btVector3(0, 1, 0), 1), 0.0, btVector3(0, -1, 0), false);
  //m_groundBody = m_bullet->createManagedRigidBody(std::make_shared<btBoxShape>(btVector3(100, 1, 100)), 0.0, btVector3(0, -1, 0), false);
//   m_sphereBody = m_bullet->createManagedRigidBody(std::make_shared<btSphereShape>(0.5f), 1.0, btVector3(-3, 2, -1), true);
//   m_sphereBody = m_bullet->createManagedRigidBody(std::make_shared<btSphereShape>(0.5f), 1.0, btVector3(3, 2, 1), true);
//   m_sphereBody = m_bullet->createManagedRigidBody(std::make_shared<btSphereShape>(0.5f), 1.0, btVector3(3, 2, -1), true);

  // AI controlled vehicles
  m_vehicles.resize(desc.numCars, 0);
  std::vector<int> internalNetworkLayers(2);
  internalNetworkLayers[0] = 4;
  internalNetworkLayers[1] = 3;
  for (int i = 0; i < desc.numCars; ++i)
  {
    m_vehicles[i] = createVehicle();
    m_vehicles[i]->initNeuralNetwork(internalNetworkLayers);
    m_vehicles[i]->setControllerNeuralNet();
  }

  
  // user controller vehicle
//   m_vehicleUser = createVehicle();
//   m_vehicleUser->setControllerUser(m_app);


  m_evolution = new EvolutionProcess(0.25f, 1.0f, 0.05f);


  initTrack();

  // set callback for collision between vehicle chassis and terrain
  m_bullet->world->setInternalTickCallback(subtickCallback, this);
}

Simulation::~Simulation()
{
  if (m_bullet)
  {
    if (m_vehicleUser)
    {
      m_bullet->world->removeRigidBody(m_vehicleUser->physics()->getRigidBody());
      m_bullet->world->removeVehicle(m_vehicleUser->physics());
      delete m_vehicleUser;
    }

    delete m_bullet;
  }

  for (size_t i = 0; i < m_vehicles.size(); ++i)
    delete m_vehicles[i];

  for (size_t i = 0; i < m_chromosomes.size(); ++i)
  {
    delete m_chromosomes[i];
    delete m_chromosomesNext[i];
  }

  delete m_evolution;
}


void Simulation::initTweakVars(TwBar* bar)
{
  if (m_evolution)
    m_evolution->initTweakVars(bar);

  TwAddVarRW(bar, "MutChange", TW_TYPE_FLOAT, &Vehicle::Chromosome::mutationMaxChange, "min=0 max=10 step=0.01 group=Evolution");
}

void Simulation::subtickCallback(btDynamicsWorld* world, btScalar timeStep)
{
  Simulation* sim = static_cast<Simulation*>(world->getWorldUserInfo());

  int numManifolds = world->getDispatcher()->getNumManifolds();
  for (int i = 0; i < numManifolds; i++)
  {
    btPersistentManifold* contactManifold = world->getDispatcher()->getManifoldByIndexInternal(i);
    const btCollisionObject* obA = contactManifold->getBody0();
    const btCollisionObject* obB = contactManifold->getBody1();

    int numContacts = contactManifold->getNumContacts();
    for (int j = 0; j < numContacts; j++)
    {
      btManifoldPoint& pt = contactManifold->getContactPoint(j);
      if (pt.getDistance() < 0.f)
      {
        // check if track is colliding
        if (obB == sim->m_trackBody)
          std::swap(obB, obA);

        if (obA == sim->m_trackBody)
        {
          // check if other colliding body is a vehicle

          for (int i = 0; i < sim->numVehicles(); ++i)
          {
            Vehicle* v = sim->vehicle(i);

            if (obB == v->physics()->getRigidBody())
            {
              v->kill();
              break;
            }
          }
        }
      }
    }
  }
}

void Simulation::update(double dt)
{
  // update bullet world
  m_bullet->world->stepSimulation(static_cast<btScalar>(dt), 10);



  // update evolution process
  int numAlive = 0;

  size_t n = m_vehicles.size();
  for (int i = 0; i < n; ++i)
  {
    Vehicle* v = m_vehicles[i];

    if (v->alive())
    {
      v->update(dt, this);

      // kill vehicles in reverse dir
      if (v->curTrackSegment() < 0)
        v->kill();

      // kill vehicles that don't make any progress
      if ((glfwGetTime() - v->birthTime()) > 20.0f && v->curTrackSegment() < 2)
        v->kill();
    }

    if (v->alive())
      ++numAlive;
    else
      v->physics()->getRigidBody()->forceActivationState(DISABLE_SIMULATION);
  }


  if (!numAlive)
  {
    applyEvolution();

    resetVehicles();
  }
}


Vehicle* Simulation::bestVehicle() const
{
  float fitness = -1.0f;
  Vehicle* v = 0;

  for (size_t i = 0; i < m_vehicles.size(); ++i)
  {
    float d = m_vehicles[i]->curTrackDistance();
    if (d > fitness && m_vehicles[i]->alive())
    {
      fitness = d;
      v = m_vehicles[i];
    }
  }

  return v;
}

void Simulation::initTrack()
{
  // load from heightfield
  unsigned int w, h;
  if (!lodepng::decode(m_trackHeights, w, h, m_desc.trackHeightsFilename, LCT_GREY, 8u))
  {
    std::shared_ptr<btHeightfieldTerrainShape> trackShape = std::make_shared<btHeightfieldTerrainShape>(w, h, &m_trackHeights[0], 10.0f / 256.0f, 0.0f, 10.0f, 1, PHY_UCHAR, false);

    trackShape->setLocalScaling(btVector3(m_desc.trackScale, m_desc.trackScale, m_desc.trackScale));

    // bullet shifts the heightfield, so that the aabb center is the origin
    btTransform idt;
    idt.setIdentity();
    btVector3 aabbMin, aabbMax;
    trackShape->getAabb(idt, aabbMin, aabbMax);
    btVector3 diag = (aabbMax - aabbMin);

    // shift to ground level
    btVector3 shift(0.0f, diag[1] * 0.5f + m_desc.trackGroundLevel, 0.0f);

    m_trackBody = m_bullet->createManagedRigidBody(trackShape, 0.0f, shift, false);
  }
  else
    std::cout << "failed to load track heightmap" << std::endl;


  // load segments
  m_trackSegments.clear();
  std::ifstream file(m_desc.trackSegmentsFilename);
  if (file.is_open())
  {
    for (std::string line; std::getline(file, line);)
    {
      float v[3];
      if (sscanf(line.c_str(), "v %f %f %f", v, v+1, v+2) == 3)
      {
        m_trackSegments.push_back(btVector3(v[0], v[1], v[2]) * m_desc.trackScale);
        m_trackSegments.back()[0] *= static_cast<float>(w);
        m_trackSegments.back()[2] *= static_cast<float>(h);
        m_trackSegments.back()[1] += m_desc.trackGroundLevel;
      }
    }

    file.close();


    std::reverse(m_trackSegments.begin(), m_trackSegments.end());


    // compute accumulated segment distance
    m_trackSegmentDist.resize(m_trackSegments.size(), 0.0f);
    float accum = 0.0f;
    for (size_t i = 1; i < m_trackSegments.size(); ++i)
    {
      accum += (m_trackSegments[i] - m_trackSegments[i - 1]).norm();
      m_trackSegmentDist[i] = accum;
    }

  }
  else
    std::cout << "failed to load track heightmap" << std::endl;
}

void Simulation::initSensors()
{
  m_sensorConfigStart.resize(3);
  m_sensorConfigEnd.resize(3);

  float config[] =
  {
    0.0209f, 1.5000f, 1.0072f,
    0.0209f, 1.5000f, 5.0666f,
    -0.5070f, 1.5000f, 0.9990f,
    -3.1516f, 1.5000f, 4.2972f,
    0.4965f, 1.5000f, 1.0095f,
    3.3649f, 1.5000f, 4.4035f
  };

//   float config[] =
//   {
//     0,0,0,  1,0,0,
//     0,0,0,  0,1,0,
//     0,0,0,  0,0,1
//   };

  float scale = 5.0f;

  for (int i = 0; i < 3; ++i)
  { 
    m_sensorConfigStart[i] = btVector3(config[i * 6 + 0], config[i * 6 + 1], config[i * 6 + 2]);
    m_sensorConfigEnd[i] = btVector3(config[i * 6 + 3], config[i * 6 + 4], config[i * 6 + 5]);

    m_sensorConfigEnd[i] = m_sensorConfigStart[i] + (m_sensorConfigEnd[i] - m_sensorConfigStart[i]) * scale;
  }
}

Vehicle* Simulation::createVehicle()
{
  btRaycastVehicle* bvehicle = createVehiclePhysics();
  Vehicle* vehicle = new Vehicle(bvehicle);

  if (m_sensorConfigStart.empty())
    initSensors();

  for (size_t i = 0; i < m_sensorConfigStart.size(); ++i)
    vehicle->addSensor(m_sensorConfigStart[i], m_sensorConfigEnd[i]);

  return vehicle;
}


btRaycastVehicle* Simulation::createVehiclePhysics()
{
  if (!m_vehicleChassisShape.get())
  {
    m_vehicleChassisExtents = btVector3(1, 0.5, 2);
    m_vehicleChassisOffset = btVector3(0.0, 1.0, 0.0);

    //The btBoxShape is centered at the origin
    m_vehicleChassisShape = std::make_shared<btBoxShape>(m_vehicleChassisExtents);

    //A compound shape is used so we can easily shift the center of gravity of our vehicle to its bottom
    //This is needed to make our vehicle more stable
    m_vehicleChassisCompound = std::make_shared<btCompoundShape>();

    btTransform localTransform;
    localTransform.setIdentity();
    localTransform.setOrigin(m_vehicleChassisOffset);

    //The center of gravity of the compound shape is the origin. When we add a rigidbody to the compound shape
    //it's center of gravity does not change. This way we can add the chassis rigidbody one unit above our center of gravity
    //keeping it under our chassis, and not in the middle of it
    m_vehicleChassisCompound->addChildShape(localTransform, m_vehicleChassisShape.get());
  }

  return m_bullet->createUnmanagedVehicle(m_vehicleChassisCompound, 1200, btVector3(0.0, 1.0, 0.0), Vehicle::collisionGroup(), ~Vehicle::collisionGroup());
}

void Simulation::applyEvolution()
{
  size_t n = m_vehicles.size();
  if (m_chromosomes.empty())
  {
    m_chromosomes.resize(n, 0);
    m_chromosomesNext.resize(n, 0);

    for (size_t i = 0; i < n; ++i)
    {
      m_chromosomes[i] = new Vehicle::Chromosome(m_vehicles[i], &m_avgDrivenDistance);
      m_chromosomesNext[i] = new Vehicle::Chromosome(m_vehicles[i], &m_avgDrivenDistance);
    }
  }

  m_avgDrivenDistance = 0.0f;
  for (size_t i = 0; i < n; ++i)
  {
    Vehicle::Chromosome* c = dynamic_cast<Vehicle::Chromosome*>(m_chromosomes[i]);
    c->readGenesFromVehicle();

    m_avgDrivenDistance += c->vehicle->curTrackDistance();
  }
  m_avgDrivenDistance /= static_cast<float>(n);

  m_evolution->computeNewPopulation(m_chromosomes, m_chromosomesNext);
  std::swap(m_chromosomes, m_chromosomesNext);
  
  for (size_t i = 0; i < n; ++i)
  {
    Vehicle::Chromosome* c = dynamic_cast<Vehicle::Chromosome*>(m_chromosomes[i]);
    c->transferGenesToVehicle();
  }
}

void Simulation::resetVehicles()
{
  size_t n = m_vehicles.size();
  for (size_t i = 0; i < n; ++i)
  {
    Vehicle* v = m_vehicles[i];
    
    v->reset();

    // replace with new bullet raycast vehicle
    btRaycastVehicle* vphysics = createVehiclePhysics();
    v->replacePhysics(vphysics, m_bullet->world);
  }
}

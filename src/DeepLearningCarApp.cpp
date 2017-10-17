#include "DeepLearningCarApp.h"


#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>


#include <iostream>
#include <vector>


DeepLearningCarApp::DeepLearningCarApp(int w, int h, int glMajor, int glMinor, double physicsTimeStep)
  : Application(w, h, glMajor, glMinor, physicsTimeStep),
  m_cam(0), m_camControl(0)  
{
  m_cam = new Camera();

  // user controlled camera
  CameraControllerUser* camUserControl = new CameraControllerUser(m_cam);
  m_camControl = camUserControl;
  addUserInputController(camUserControl);

  // pass window events to tweakbar
  static AntTweakbarInputController tweakbarController;
  addUserInputController(&tweakbarController);
}


DeepLearningCarApp::~DeepLearningCarApp()
{
  delete m_camControl;
  delete m_cam;

  TwTerminate();
}

void DeepLearningCarApp::init()
{
  // init AntTweakBar
  TwInit(TW_OPENGL, NULL);
  int w, h;
  glfwGetWindowSize(window(), &w, &h);
  TwWindowSize(w, h);


  Simulation::Desc desc;
  desc.numCars = 40;
  desc.trackHeightsFilename = "../data/tracks/track0.png";
  desc.trackSegmentsFilename = "../data/tracks/track0_segments.obj";
  desc.trackScale = 2.0f;

  m_simulation = new Simulation(desc, this);
  
  // ----------------------------------------------------

  m_cam->perspective(90.0f * 3.14159f / 180.0f, static_cast<float>(w) / h, 0.0001f, 100.0f);

  // ----------------------------------------------------

//   m_vehicleController = new VehicleControllerUser(m_vehicle);
//   addUserInputController(m_vehicleController);

  // use vehicle follow cam
  if (dynamic_cast<UserInputController*>(m_camControl))
    removeUserInputController(dynamic_cast<UserInputController*>(m_camControl));
  delete m_camControl;
  m_camControl = new CameraControllerFollow(m_cam, m_simulation->vehicle(0)->physics()->getRigidBody());

  // -----------------------------------------------------

  m_renderer = new Renderer();


  m_simulation->initTweakVars(m_renderer->tweakbar());
}

void DeepLearningCarApp::draw(double time)
{
  int width, height;
  framebufferSize(&width, &height);

  m_renderer->framebufferSize(width, height);

  m_renderer->draw(time, m_simulation, m_cam);
}

void DeepLearningCarApp::updatePhysics(double dt)
{
  if (m_simulation)
    m_simulation->update(dt);

  // follow best vehicle
  CameraControllerFollow* followCam = dynamic_cast<CameraControllerFollow*>(m_camControl);
  if (followCam)
  {
    Vehicle* best = m_simulation->bestVehicle();

    if (best)
      followCam->body(best->physics()->getRigidBody());
  }

  // animate camera
  if (m_camControl)
    m_camControl->update(dt);
}




#include "optifuser_controller.h"
#include "articulation/sapien_articulation.h"
#include "articulation/sapien_joint.h"
#include "articulation/sapien_link.h"
#include "render_interface.h"
#include "sapien_actor.h"
#include "sapien_drive.h"
#include "sapien_scene.h"
#include "simulation.h"
#include <spdlog/spdlog.h>
#define IMGUI_IMPL_OPENGL_LOADER_GLEW
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <glm/gtx/matrix_decompose.hpp>
// After ImGui
#include "ImGuizmo.h"

constexpr int WINDOW_WIDTH = 1200, WINDOW_HEIGHT = 800;
static const uint32_t imguiWindowSize = 300;

glm::mat4 PxTransform2Mat4(physx::PxTransform const &t) {
  glm::mat4 rot = glm::toMat4(glm::quat(t.q.w, t.q.x, t.q.y, t.q.z));
  glm::mat4 pos = glm::translate(glm::mat4(1.f), {t.p.x, t.p.y, t.p.z});
  return pos * rot;
}

physx::PxTransform Mat42PxTransform(glm::mat4 const &m) {
  glm::vec3 scale;
  glm::quat rot;
  glm::vec3 pos;
  glm::vec3 skew;
  glm::vec4 perspective;
  glm::decompose(m, scale, rot, pos, skew, perspective);
  return {{pos.x, pos.y, pos.z}, {rot.x, rot.y, rot.z, rot.w}};
}

enum RenderMode {
  LIGHTING,
  ALBEDO,
  NORMAL,
  DEPTH,
  SEGMENTATION,
  CUSTOM
#ifdef _USE_OPTIX
  ,
  PATHTRACER
#endif
};

namespace sapien {
namespace Renderer {

static int pickedId = 0;
static int pickedRenderId = 0;

void OptifuserController::editTransform() {
  static ImGuizmo::OPERATION mCurrentGizmoOperation(ImGuizmo::TRANSLATE);
  static ImGuizmo::MODE mCurrentGizmoMode(ImGuizmo::LOCAL);
  static bool useSnap = false;
  static float snap[3] = {1.f, 1.f, 1.f};
  static float bounds[] = {-0.5f, -0.5f, -0.5f, 0.5f, 0.5f, 0.5f};
  static float boundsSnap[] = {0.1f, 0.1f, 0.1f};
  static bool boundSizing = false;
  static bool boundSizingSnap = false;

  ImGui::SetNextWindowPos(ImVec2(imguiWindowSize, 0));
  ImGui::SetNextWindowSize(ImVec2(mRenderer->mContext->getWidth() - 2 * imguiWindowSize, 0));

  ImGui::Begin("Gizmo");
  if (ImGui::RadioButton("Translate", mCurrentGizmoOperation == ImGuizmo::TRANSLATE))
    mCurrentGizmoOperation = ImGuizmo::TRANSLATE;
  ImGui::SameLine();
  if (ImGui::RadioButton("Rotate", mCurrentGizmoOperation == ImGuizmo::ROTATE))
    mCurrentGizmoOperation = ImGuizmo::ROTATE;
  float matrixTranslation[3], matrixRotation[3], matrixScale[3] = {1, 1, 1};
  ImGuizmo::DecomposeMatrixToComponents(&gizmoTransform[0][0], matrixTranslation, matrixRotation,
                                        matrixScale);
  ImGui::InputFloat3("Tr", matrixTranslation, 3);
  ImGui::InputFloat3("Rt", matrixRotation, 3);
  ImGuizmo::RecomposeMatrixFromComponents(matrixTranslation, matrixRotation, matrixScale,
                                          &gizmoTransform[0][0]);

  if (ImGui::RadioButton("Local", mCurrentGizmoMode == ImGuizmo::LOCAL))
    mCurrentGizmoMode = ImGuizmo::LOCAL;
  if (ImGui::RadioButton("World", mCurrentGizmoMode == ImGuizmo::WORLD))
    mCurrentGizmoMode = ImGuizmo::WORLD;
  ImGui::Checkbox("", &useSnap);
  ImGui::SameLine();

  switch (mCurrentGizmoOperation) {
  case ImGuizmo::TRANSLATE:
    ImGui::InputFloat3("Snap", &snap[0]);
    break;
  case ImGuizmo::ROTATE:
    ImGui::InputFloat("Angle Snap", &snap[0]);
    break;
  default:
    break;
  }

  if (ImGui::Button("Reset")) {
    gizmoTransform = glm::mat4(1);
    createGizmoVisual(nullptr);
  }
  auto pose = Mat42PxTransform(gizmoTransform);
  for (auto b : gizmoBody) {
    b->update(pose);
  }
  if (mCurrentSelection) {
    SActorBase *actor = mCurrentSelection;
    if (actor &&
        (actor->getType() == EActorType::DYNAMIC || actor->getType() == EActorType::KINEMATIC)) {
      ImGui::SameLine();
      if (ImGui::Button("Teleport Actor")) {
        static_cast<SActor *>(actor)->setPose(pose);
      }
    }

    if (actor && (actor->getType() == EActorType::DYNAMIC ||
                  actor->getType() == EActorType::ARTICULATION_LINK)) {
      ImGui::SameLine();
      if (ImGui::Button("Drive Actor")) {
        SDrive *validDrive = nullptr;
        auto drives = actor->getDrives();
        for (SDrive *d : drives) {
          if (d->getActor1() == nullptr &&
              d->getLocalPose1() == PxTransform({{0, 0, 0}, PxIdentity}) &&
              d->getLocalPose2() == PxTransform({{0, 0, 0}, PxIdentity})) {
            validDrive = d;
          }
        }
        if (!validDrive) {
          validDrive = mScene->createDrive(nullptr, {{0, 0, 0}, PxIdentity}, actor,
                                           {{0, 0, 0}, PxIdentity});
          validDrive->setProperties(10000, 10000, PX_MAX_F32, false);
        }
        validDrive->setTarget(pose);
      }
    }
  }

  ImGuiIO &io = ImGui::GetIO();
  ImGuizmo::SetRect(0, 0, io.DisplaySize.x, io.DisplaySize.y);
  auto view = mCamera->getViewMat();
  auto proj = mCamera->getProjectionMat();
  ImGuizmo::Manipulate(&view[0][0], &proj[0][0], mCurrentGizmoOperation, mCurrentGizmoMode,
                       &gizmoTransform[0][0], NULL, useSnap ? &snap[0] : NULL,
                       boundSizing ? bounds : NULL, boundSizingSnap ? boundsSnap : NULL);
  ImGui::End();
}

void OptifuserController::createGizmoVisual(SActorBase *actor) {
  if (mScene) {
    for (auto b : gizmoBody) {
      b->destroy();
    }
    gizmoBody.clear();
    if (actor) {
      for (auto b : actor->getRenderBodies()) {
        auto body = static_cast<OptifuserRigidbody *>(b);
        auto scene = static_cast<OptifuserScene *>(mScene->getRendererScene());
        gizmoBody.push_back(scene->cloneRigidbody(body));
        gizmoBody.back()->setUniqueId(0);
        gizmoBody.back()->setRenderMode(2);
      }
    }
  }
}

OptifuserController::OptifuserController(OptifuserRenderer *renderer)
    : mRenderer(renderer), mCameraMode(0),
      mCamera(std::make_unique<Optifuser::PerspectiveCameraSpec>()),
      mFreeCameraController(mCamera.get()), mArcCameraController(mCamera.get()) {
  mCamera->aspect = WINDOW_WIDTH / (float)WINDOW_HEIGHT;
  setCameraPosition(0, 0, 1);
  setCameraRotation(0, 0);
}
void OptifuserController::showWindow() { mRenderer->mContext->showWindow(); }
void OptifuserController::hideWindow() { mRenderer->mContext->hideWindow(); }

void OptifuserController::setCurrentScene(SScene *scene) { mScene = scene; }
void OptifuserController::focus(SActorBase *actor) {
  if (actor && !mCurrentFocus) {
    // none -> focus
    mArcCameraController.yaw = mFreeCameraController.yaw;
    mArcCameraController.pitch = mFreeCameraController.pitch;
    auto p = actor->getPose().p;
    mArcCameraController.center = {p.x, p.y, p.z};
    mArcCameraController.r = glm::length(glm::vec3(
        mCamera->position.x - p.x, mCamera->position.y - p.y, mCamera->position.z - p.z));
    actor->EventEmitter<EventActorPreDestroy>::registerListener(*this);
  } else if (!actor && mCurrentFocus) {
    // focus -> none
    mFreeCameraController.yaw = mArcCameraController.yaw;
    mFreeCameraController.pitch = mArcCameraController.pitch;
    auto &p = mArcCameraController.camera->position;
    mFreeCameraController.setPosition(p.x, p.y, p.z);
    if (mCurrentSelection != mCurrentFocus) {
      mCurrentFocus->EventEmitter<EventActorPreDestroy>::unregisterListener(*this);
    }
  } else if (actor && actor != mCurrentFocus) {
    // focus1 -> focus2
    if (mCurrentSelection != mCurrentFocus) {
      mCurrentFocus->EventEmitter<EventActorPreDestroy>::unregisterListener(*this);
    }
    actor->EventEmitter<EventActorPreDestroy>::registerListener(*this);
  } // none -> none
  mCurrentFocus = actor;
}

void OptifuserController::select(SActorBase *actor) {
  if (actor != mCurrentSelection) {
    if (mCurrentSelection) {
      for (auto b : mCurrentSelection->getRenderBodies()) {
        b->setRenderMode(0);
      }
    }
    if (mCurrentSelection && mCurrentSelection != mCurrentFocus) {
      mCurrentSelection->EventEmitter<EventActorPreDestroy>::unregisterListener(*this);
    }
    if (actor) {
      actor->EventEmitter<EventActorPreDestroy>::registerListener(*this);
      if (transparentSelection) {
        for (auto b : actor->getRenderBodies()) {
          b->setRenderMode(2);
        }
      }
    }
    mCurrentSelection = actor;
  }
}

void OptifuserController::setCameraPosition(float x, float y, float z) {
  focus(nullptr);
  mFreeCameraController.setPosition(x, y, z);
}

void OptifuserController::setCameraRotation(float yaw, float pitch) {
  focus(nullptr);
  mFreeCameraController.yaw = yaw;
  mFreeCameraController.pitch = pitch;
  mFreeCameraController.update();
}

void OptifuserController::setCameraOrthographic(bool ortho) {
  if (ortho) {
    mCameraMode = 1;
    auto cam = std::make_unique<Optifuser::OrthographicCameraSpec>();
    cam->name = mCamera->name;
    cam->aspect = mCamera->aspect;
    cam->position = mCamera->position;
    cam->setRotation(mCamera->getRotation());
    cam->near = mCamera->near;
    cam->far = mCamera->far;
    cam->scaling = 1.f;
    mCamera = std::move(cam);
  } else {
    mCameraMode = 0;
    auto cam = std::make_unique<Optifuser::PerspectiveCameraSpec>();
    cam->name = mCamera->name;
    cam->aspect = mCamera->aspect;
    cam->position = mCamera->position;
    cam->setRotation(mCamera->getRotation());
    cam->near = mCamera->near;
    cam->far = mCamera->far;
    cam->fovy = glm::radians(35.f);
    mCamera = std::move(cam);
  }
  mFreeCameraController.changeCamera(mCamera.get());
  mArcCameraController.changeCamera(mCamera.get());
}

physx::PxTransform OptifuserController::getCameraPose() const {
  auto p = mCamera->position;
  auto q = mCamera->getRotation();
  return {{p.x, p.y, p.z}, {q.x, q.y, q.z, q.w}};
}

bool OptifuserController::shouldQuit() { return mShouldQuit; }

void OptifuserController::render() {
  do {
#ifdef _USE_OPTIX
    static Optifuser::OptixRenderer *pathTracer = nullptr;
#endif

    static int renderMode = 0;
    static float moveSpeed = 3.f;
    mRenderer->mContext->processEvents();
    float framerate = ImGui::GetIO().Framerate;

    float dt = 1.f * moveSpeed / framerate;

    if (Optifuser::getInput().getKeyState(GLFW_KEY_W)) {
      focus(nullptr);
      mFreeCameraController.moveForwardRight(dt, 0);
#ifdef _USE_OPTIX
      if (renderMode == PATHTRACER) {
        pathTracer->invalidateCamera();
      }
#endif
    } else if (Optifuser::getInput().getKeyState(GLFW_KEY_S)) {
      focus(nullptr);
      mFreeCameraController.moveForwardRight(-dt, 0);
#ifdef _USE_OPTIX
      if (renderMode == PATHTRACER) {
        pathTracer->invalidateCamera();
      }
#endif
    } else if (Optifuser::getInput().getKeyState(GLFW_KEY_A)) {
      focus(nullptr);
      mFreeCameraController.moveForwardRight(0, -dt);
#ifdef _USE_OPTIX
      if (renderMode == PATHTRACER) {
        pathTracer->invalidateCamera();
      }
#endif
    } else if (Optifuser::getInput().getKeyState(GLFW_KEY_D)) {
      focus(nullptr);
      mFreeCameraController.moveForwardRight(0, dt);
#ifdef _USE_OPTIX
      if (renderMode == PATHTRACER) {
        pathTracer->invalidateCamera();
      }
#endif
    }

    if (mCurrentFocus) {
      auto [x, y, z] = mCurrentFocus->getPose().p;

      double dx, dy;
      Optifuser::getInput().getWheelDelta(dx, dy);

      mArcCameraController.r += dy;
      if (mArcCameraController.r < 1) {
        mArcCameraController.r = 1;
      }
      mArcCameraController.setCenter(x, y, z);

#ifdef _USE_OPTIX
      if ((dx != 0 || dy != 0) && renderMode == PATHTRACER) {
        pathTracer->invalidateCamera();
      }
#endif
    }

    if (Optifuser::getInput().getKeyDown(GLFW_KEY_Q)) {
      mShouldQuit = true;
    }

    mCamera->aspect = static_cast<float>(mRenderer->mContext->getWidth()) /
                      static_cast<float>(mRenderer->mContext->getHeight());

    mCamera->aspect = static_cast<float>(mRenderer->mContext->getWidth()) /
                      static_cast<float>(mRenderer->mContext->getHeight());

    static bool renderGui = true;
    if (Optifuser::getInput().getKeyDown(GLFW_KEY_E)) {
      renderGui = !renderGui;
    }
    if (Optifuser::getInput().getMouseButton(GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS) {
      double dx, dy;
      Optifuser::getInput().getCursorDelta(dx, dy);
      if (flipX) {
        dx = -dx;
      }
      if (flipY) {
        dy = -dy;
      }
      if (!mCurrentFocus) {
        mFreeCameraController.rotateYawPitch(-dx / 1000.f, -dy / 1000.f);
      } else {
        mArcCameraController.rotateYawPitch(-dx / 1000.f, -dy / 1000.f);
      }
#ifdef _USE_OPTIX
      if (renderMode == PATHTRACER) {
        pathTracer->invalidateCamera();
      }
#endif
    }

    OptifuserScene *currentScene = nullptr;
    if (mScene) {
      currentScene = static_cast<OptifuserScene *>(mScene->getRendererScene());
    }

    if (currentScene) {
      mRenderer->mContext->renderer.renderScene(*currentScene->getScene(), *mCamera);

      if (renderMode == LIGHTING) {
        mRenderer->mContext->renderer.displayLighting();
      } else if (renderMode == SEGMENTATION) {
        mRenderer->mContext->renderer.displaySegmentation();
      } else if (renderMode == CUSTOM) {
        mRenderer->mContext->renderer.displayUserTexture();
#ifdef _USE_OPTIX
      } else if (renderMode == PATHTRACER) {
        // path tracer
        pathTracer->numRays = 4;
        pathTracer->max_iterations = 100000;
        pathTracer->renderScene(*currentScene->getScene(), *mCamera);
        pathTracer->display();
#endif
      } else {
        mRenderer->mContext->renderer.display();
      }
    }

    if (Optifuser::getInput().getMouseDown(GLFW_MOUSE_BUTTON_LEFT)) {
      int x, y;
      Optifuser::getInput().getCursor(x, y);
      pickedId = mRenderer->mContext->renderer.pickSegmentationId(x, y);

      pickedRenderId = 0;
      if (pickedId) {
        pickedRenderId = mRenderer->mContext->renderer.pickObjectId(x, y);
      }

      SActorBase *actor = mScene->findActorById(pickedId);
      if (!actor) {
        actor = mScene->findArticulationLinkById(pickedId);
      }
      select(actor);
    }

    if (mCurrentSelection) {
      SActorBase *actor = mCurrentSelection;
      PxTransform cmPose = PxTransform({0, 0, 0}, PxIdentity);

      switch (actor->getType()) {
      case EActorType::DYNAMIC:
      case EActorType::KINEMATIC:
      case EActorType::ARTICULATION_LINK:
        cmPose = actor->getPxActor()->getGlobalPose() *
                 static_cast<PxRigidBody *>(actor->getPxActor())->getCMassLocalPose();
        break;
      default:
        break;
      }

      if (Optifuser::getInput().getKeyDown(GLFW_KEY_F)) {
        focus(actor);
      }

      currentScene->getScene()->clearAxes();

      auto pos = showCM ? cmPose.p : actor->getPxActor()->getGlobalPose().p;
      auto quat = showCM ? cmPose.q : actor->getPxActor()->getGlobalPose().q;
      currentScene->getScene()->addAxes({pos.x, pos.y, pos.z}, {quat.w, quat.x, quat.y, quat.z});
    }

    static int camIndex = -1;
    int changeShader = 0;
    if (renderGui) {
      ImGui_ImplOpenGL3_NewFrame();
      ImGui_ImplGlfw_NewFrame();

      ImGui::NewFrame();
      if (gizmo) {
        ImGuizmo::BeginFrame();
        editTransform();
      }

      ImGui::SetNextWindowPos(ImVec2(0, 0));
      ImGui::SetNextWindowSize(ImVec2(imguiWindowSize, mRenderer->mContext->getHeight()));

      ImGui::Begin("Render Options");
      {
        if (ImGui::CollapsingHeader("Control", ImGuiTreeNodeFlags_DefaultOpen)) {
          ImGui::Checkbox("Pause", &paused);
          ImGui::Checkbox("Flip X", &flipX);
          ImGui::Checkbox("Flip Y", &flipY);
          ImGui::Checkbox("Transparent Selection", &transparentSelection);
          ImGui::Checkbox("Show Gizmo", &gizmo);
        }
        if (ImGui::CollapsingHeader("Render Mode", ImGuiTreeNodeFlags_DefaultOpen)) {
          if (ImGui::RadioButton("Lighting", &renderMode, RenderMode::LIGHTING)) {
          };
          if (ImGui::RadioButton("Albedo", &renderMode, RenderMode::ALBEDO)) {
            changeShader = 1;
          }
          if (ImGui::RadioButton("Normal", &renderMode, RenderMode::NORMAL)) {
            changeShader = 1;
          }
          if (ImGui::RadioButton("Depth", &renderMode, RenderMode::DEPTH)) {
            changeShader = 1;
          }
          if (ImGui::RadioButton("Segmentation", &renderMode, RenderMode::SEGMENTATION)) {
          }
          if (ImGui::RadioButton("Custom", &renderMode, RenderMode::CUSTOM)) {
          }
#ifdef _USE_OPTIX
          if (ImGui::RadioButton("PathTracer", &renderMode, RenderMode::PATHTRACER)) {
            if (pathTracer) {
              delete pathTracer;
            }
            pathTracer = new Optifuser::OptixRenderer(OptifuserRenderer::gPtxDir);
            pathTracer->setBlackBackground();
            pathTracer->init(mRenderer->mContext->getWidth(), mRenderer->mContext->getHeight());
            pathTracer->enableDenoiser();
          } else {
          }
#endif
        }

        if (ImGui::CollapsingHeader("Main Camera", ImGuiTreeNodeFlags_DefaultOpen)) {

          static bool ortho;
          if (ImGui::Checkbox("Orthographic", &ortho)) {
            if (ortho) {
              setCameraOrthographic(true);
            } else {
              setCameraOrthographic(false);
            }
          }

          ImGui::Text("Position");
          ImGui::Text("%-4.3f %-4.3f %-4.3f", mCamera->position.x, mCamera->position.y,
                      mCamera->position.z);
          ImGui::Text("Forward");
          auto forward = mCamera->getRotation() * glm::vec3(0, 0, -1);
          ImGui::Text("%-4.3f %-4.3f %-4.3f", forward.x, forward.y, forward.z);

          if (mCameraMode == 0) {
            ImGui::Text("Fov");
            auto fovy = &static_cast<Optifuser::PerspectiveCameraSpec *>(mCamera.get())->fovy;
            ImGui::SliderAngle("##fov(y)", fovy, 1.f, 90.f);
          } else {
            ImGui::Text("Scaling");
            auto scaling =
                &static_cast<Optifuser::OrthographicCameraSpec *>(mCamera.get())->scaling;
            ImGui::SliderFloat("##scaling", scaling, 0.1f, 10.f);
          }
          ImGui::Text("Move speed");
          ImGui::SliderFloat("##speed", &moveSpeed, 1.f, 10.f);
          ImGui::Text("Width: %d", mRenderer->mContext->getWidth());
          ImGui::SameLine();
          ImGui::Text("Height: %d", mRenderer->mContext->getHeight());
          ImGui::SameLine();
          ImGui::Text("Aspect: %.2f", mCamera->aspect);
          ImGui::Text("Picked link id: %d", pickedId);
          ImGui::Text("Picked render id: %d", pickedRenderId);
        }

        if (ImGui::CollapsingHeader("Mounted Cameras")) {
          ImGui::RadioButton("None##camera", &camIndex, -1);

          if (currentScene) {
            auto cameras = currentScene->getCameras();
            for (uint32_t i = 0; i < cameras.size(); ++i) {
              ImGui::RadioButton((cameras[i]->getName() + "##camera" + std::to_string(i)).c_str(),
                                 &camIndex, i);
            }

            // handle camera deletion
            if (camIndex >= static_cast<int>(cameras.size())) {
              camIndex = -1;
            }

            if (camIndex >= 0) {
              uint32_t width = cameras[camIndex]->getWidth();
              uint32_t height = cameras[camIndex]->getHeight();
              cameras[camIndex]->takePicture();
              ImGui::Image(
                  reinterpret_cast<ImTextureID>(static_cast<OptifuserCamera *>(cameras[camIndex])
                                                    ->mRenderContext->renderer.lightingtex),
                  ImVec2(imguiWindowSize, imguiWindowSize / static_cast<float>(width) * height),
                  ImVec2(0, 1), ImVec2(1, 0));
            }
          }
        }

        if (ImGui::CollapsingHeader("Stats", ImGuiTreeNodeFlags_DefaultOpen)) {
          ImGui::Text("Frame Time: %.3f ms/frame (%.1f FPS)", 1000.0f / ImGui::GetIO().Framerate,
                      ImGui::GetIO().Framerate);
        }
      }
      ImGui::End();

      SArticulationBase *articulation = nullptr;
      ImGui::SetNextWindowPos(ImVec2(mRenderer->mContext->getWidth() - imguiWindowSize, 0));
      ImGui::SetNextWindowSize(ImVec2(imguiWindowSize, mRenderer->mContext->getHeight()));
      ImGui::Begin("Object Properties");
      if (ImGui::CollapsingHeader("Global")) {
        auto flags = mScene->getPxScene()->getFlags();

        bool b = flags & PxSceneFlag::eENABLE_ENHANCED_DETERMINISM;
        ImGui::Checkbox("Enhanced determinism", &b);
        b = flags & PxSceneFlag::eENABLE_PCM;
        ImGui::Checkbox("PCM(persistent contact manifold)", &b);
        b = flags & PxSceneFlag::eENABLE_CCD;
        ImGui::Checkbox("CCD(continuous collision detection)", &b);
        b = flags & PxSceneFlag::eENABLE_STABILIZATION;
        ImGui::Checkbox("Stabilization", &b);
        b = flags & PxSceneFlag::eENABLE_AVERAGE_POINT;
        ImGui::Checkbox("Average point", &b);
        b = flags & PxSceneFlag::eENABLE_GPU_DYNAMICS;
        ImGui::Checkbox("GPU dynamics", &b);
        b = flags & PxSceneFlag::eENABLE_FRICTION_EVERY_ITERATION;
        ImGui::Checkbox("Friction in every solver iteration", &b);
        b = flags & PxSceneFlag::eADAPTIVE_FORCE;
        ImGui::Checkbox("Adaptive force", &b);

        ImGui::Text("Contact offset: %.4f", mScene->getDefaultContactOffset());
        ImGui::Text("Sleep threshold: %.4f", mScene->getDefaultSleepThreshold());
        ImGui::Text("Solver iterations: %d", mScene->getDefaultSolverIterations());
        ImGui::Text("Solver velocity iterations: %d", mScene->getDefaultSolverVelocityIterations());
      }
      if (ImGui::CollapsingHeader("World")) {
        ImGui::Text("Scene: %s", mScene->getName().c_str());
        if (ImGui::TreeNode("Actors")) {
          auto actors = mScene->getAllActors();
          for (uint32_t i = 0; i < actors.size(); ++i) {
            std::string name = actors[i]->getName();
            if (name.empty()) {
              name = "(no name)";
            }
            if (actors[i] == mCurrentSelection) {
              ImGui::TextColored({1, 0, 0, 1}, "%s", name.c_str());
            } else {
              if (ImGui::Selectable((name + "##actor" + std::to_string(i)).c_str())) {
                select(actors[i]);
              }
            }
          }
          ImGui::TreePop();
        }
        if (ImGui::TreeNode("Articulations")) {
          auto articulations = mScene->getAllArticulations();
          for (uint32_t i = 0; i < articulations.size(); ++i) {
            std::string name = articulations[i]->getName();
            if (name.empty()) {
              name = "(no name)";
            }
            if (ImGui::TreeNode((name + "##articulation" + std::to_string(i)).c_str())) {
              auto links = articulations[i]->getBaseLinks();
              for (uint32_t j = 0; j < links.size(); ++j) {
                std::string name = links[j]->getName();
                if (name.empty()) {
                  name = "(no name)";
                }
                if (links[j] == mCurrentSelection) {
                  ImGui::TextColored({1, 0, 0, 1}, "%s", name.c_str());
                } else {
                  if (ImGui::Selectable(
                          (name + "##a" + std::to_string(i) + "_" + std::to_string(j)).c_str())) {
                    select(links[j]);
                  }
                }
              }
              ImGui::TreePop();
            }
          }
          ImGui::TreePop();
        }
      }
      {
        if (mCurrentSelection) {
          if (ImGui::CollapsingHeader("Actor", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Text("name: %s", mCurrentSelection->getName().c_str());
            auto pose = mCurrentSelection->getPose();
            ImGui::Text("Position: %.2f %.2f %.2f", pose.p.x, pose.p.y, pose.p.z);

            glm::vec3 angles =
                glm::eulerAngles(glm::quat(pose.q.w, pose.q.x, pose.q.y, pose.q.z)) /
                glm::pi<float>() * 180.f;
            ImGui::Text("Euler (degree): %.2f %.2f %.2f", angles.x, angles.y, angles.z);

            ImGui::Text("col1: #%08x, col2: #%08x", mCurrentSelection->getCollisionGroup1(),
                        mCurrentSelection->getCollisionGroup2());
            ImGui::Text("col3: #%08x", mCurrentSelection->getCollisionGroup3());

            if (ImGui::Button("Gizmo to Actor")) {
              gizmo = true;
              gizmoTransform = PxTransform2Mat4(mCurrentSelection->getPose());
              createGizmoVisual(mCurrentSelection);
            }
            switch (mCurrentSelection->getType()) {
            case EActorType::ARTICULATION_LINK:
              ImGui::Text("Type: Articulation Link");
              break;
            case EActorType::DYNAMIC:
              ImGui::Text("Type: Dynamic Actor");
              break;
            case EActorType::KINEMATIC:
              ImGui::Text("Type: Kinematic Actor");
              break;
            case EActorType::KINEMATIC_ARTICULATION_LINK:
              ImGui::Text("Type: Kinematic Articulation Link");
              break;
            case EActorType::STATIC:
              ImGui::Text("Type: Static");
              break;
            }

            static bool renderCollision;
            renderCollision = mCurrentSelection->getRenderMode();
            // toggle collision shape
            if (ImGui::Checkbox("Collision Shape", &renderCollision)) {
              mCurrentSelection->setRenderMode(renderCollision);
            }

            // toggle center of mass
            ImGui::Checkbox("Center of Mass", &showCM);
          }

          if (ImGui::CollapsingHeader("Actor Details")) {
            auto actor = mCurrentSelection->getPxActor();
            std::vector<PxShape *> shapes(actor->getNbShapes());
            actor->getShapes(shapes.data(), shapes.size());
            int primitives = 0;
            int meshes = 0;
            PxReal minDynamicFriction = 100;
            PxReal maxDynamicFriction = -1;
            PxReal minStaticFriction = 100;
            PxReal maxStaticFriction = -1;
            PxReal minRestitution = 100;
            PxReal maxRestitution = -1;
            for (auto s : shapes) {
              if (s->getGeometryType() == PxGeometryType::eCONVEXMESH) {
                meshes += 1;
              } else {
                primitives += 1;
              }
              std::vector<PxMaterial *> mats(s->getNbMaterials());
              s->getMaterials(mats.data(), s->getNbMaterials());
              for (auto m : mats) {
                PxReal sf = m->getStaticFriction();
                minStaticFriction = std::min(minStaticFriction, sf);
                maxStaticFriction = std::max(maxStaticFriction, sf);
                PxReal df = m->getDynamicFriction();
                minDynamicFriction = std::min(minDynamicFriction, df);
                maxDynamicFriction = std::max(maxDynamicFriction, df);
                PxReal r = m->getRestitution();
                minRestitution = std::min(minRestitution, r);
                maxRestitution = std::max(maxRestitution, r);
              }
            }
            ImGui::Text("Primitive Count: %d", primitives);
            ImGui::Text("Convex Mesh Count: %d", meshes);
            if (maxStaticFriction >= 0) {
              ImGui::Text("Static friction: %.2f - %.2f", minStaticFriction, maxStaticFriction);
              ImGui::Text("Dynamic friction: %.2f - %.2f", minDynamicFriction, maxDynamicFriction);
              ImGui::Text("Restitution : %.2f - %.2f", minRestitution, maxRestitution);
            } else {
              ImGui::Text("No Physical Material");
            }
            if (mCurrentSelection->getType() == EActorType::DYNAMIC) {
              bool b = static_cast<PxRigidDynamic*>(actor)->isSleeping();
              ImGui::Checkbox("Sleeping", &b);
            }

            if (mCurrentSelection->getDrives().size()) {
              auto drives = mCurrentSelection->getDrives();
              if (ImGui::TreeNode("Drives")) {
                for (size_t i = 0; i < drives.size(); ++i) {
                  ImGui::Text("Drive %ld", i + 1);
                  if (drives[i]->getActor2() == mCurrentSelection) {
                    if (drives[i]->getActor1()) {
                      ImGui::Text("Drive towards pose relative to actor [%s]",
                                  drives[i]->getActor1()->getName().c_str());
                    } else {
                      ImGui::Text("Drive towards absolute pose");
                    }
                  } else {
                    if (drives[i]->getActor2()) {
                      ImGui::Text("Drive other actor [%s] towards pose relative to this actor.",
                                  drives[i]->getActor1()->getName().c_str());
                    } else {
                      ImGui::Text(
                          "This drive is created by specifying world frame as the second actor, "
                          "for "
                          "best performance, consider using world frame as the first actor");
                    }
                  }
                  ImGui::NewLine();

                  {
                    // actor 1
                    if (drives[i]->getActor1() == mCurrentSelection) {
                      ImGui::Text("Actor 1: this actor");
                    } else {
                      ImGui::Text("Actor 1: %s", drives[i]->getActor1()
                                                     ? drives[i]->getActor1()->getName().c_str()
                                                     : "world frame");
                    }
                    auto pose1 = drives[i]->getLocalPose1();
                    ImGui::Text("Drive attached at");
                    ImGui::Text("Position: %.2f %.2f %.2f", pose1.p.x, pose1.p.y, pose1.p.z);
                    glm::vec3 angles1 =
                        glm::eulerAngles(glm::quat(pose1.q.w, pose1.q.x, pose1.q.y, pose1.q.z)) /
                        glm::pi<float>() * 180.f;
                    ImGui::Text("Euler (degree): %.2f %.2f %.2f", angles1.x, angles1.y, angles1.z);
                  }
                  ImGui::NewLine();

                  {
                    // actor 2
                    if (drives[i]->getActor2() == mCurrentSelection) {
                      ImGui::Text("Actor 2: this actor");
                    } else {
                      ImGui::Text("Actor 2: %s", drives[i]->getActor2()
                                                     ? drives[i]->getActor2()->getName().c_str()
                                                     : "world frame");
                    }
                    auto pose2 = drives[i]->getLocalPose2();
                    ImGui::Text("Drive attached at");
                    ImGui::Text("Position: %.2f %.2f %.2f", pose2.p.x, pose2.p.y, pose2.p.z);
                    glm::vec3 angles2 =
                        glm::eulerAngles(glm::quat(pose2.q.w, pose2.q.x, pose2.q.y, pose2.q.z)) /
                        glm::pi<float>() * 280.f;
                    ImGui::Text("Euler (degree): %.2f %.2f %.2f", angles2.x, angles2.y, angles2.z);
                  }
                  ImGui::NewLine();

                  {
                    auto target = drives[i]->getTarget();
                    auto [v, w] = drives[i]->getTargetVelocity();

                    ImGui::Text("Drive target");
                    ImGui::Text("Position: %.2f %.2f %.2f", target.p.x, target.p.y, target.p.z);
                    glm::vec3 angles = glm::eulerAngles(glm::quat(target.q.w, target.q.x,
                                                                  target.q.y, target.q.z)) /
                                       glm::pi<float>() * 180.f;
                    ImGui::Text("Euler (degree): %.2f %.2f %.2f", angles.x, angles.y, angles.z);
                    ImGui::Text("Linear Velocity: %.2f %.2f %.2f", v.x, v.y, v.z);
                    ImGui::Text("Angular Velocity: %.2f %.2f %.2f", w.x, w.y, w.z);

                    if (ImGui::Button(("Remove Drive##" + std::to_string(i)).c_str())) {
                      drives[i]->destroy();
                    }
                    ImGui::Text("Caution: Accessing a removed drive");
                    ImGui::Text("will cause crash");
                  }
                  ImGui::NewLine();
                }
                ImGui::TreePop();
              }
            }
          }

          if (mCurrentSelection->getType() == EActorType::ARTICULATION_LINK ||
              mCurrentSelection->getType() == EActorType::KINEMATIC_ARTICULATION_LINK) {
            SLinkBase *link = static_cast<SLinkBase *>(mCurrentSelection);
            articulation = link->getArticulation();

            struct JointGuiModel {
              std::string name;
              std::array<float, 2> limits;
              float value;
            };
            std::vector<JointGuiModel> jointValues;
            jointValues.resize(articulation->dof());
            uint32_t n = 0;
            auto qpos = articulation->getQpos();
            for (auto j : articulation->getBaseJoints()) {
              auto limits = j->getLimits();
              for (uint32_t i = 0; i < j->getDof(); ++i) {
                jointValues[n].name = j->getName();
                jointValues[n].limits = limits[i];
                jointValues[n].value = qpos[n];
                ++n;
              }
            }

            if (ImGui::CollapsingHeader("Articulation", ImGuiTreeNodeFlags_DefaultOpen)) {
              ImGui::Text("name: %s", link->getArticulation()->getName().c_str());
              ImGui::Text("dof: %ld", jointValues.size());
              if (articulation->getType() == EArticulationType::DYNAMIC) {
                ImGui::Text("type: Dynamic");
              } else {
                ImGui::Text("type: Kinematic");
              }

              if (ImGui::TreeNode("Joints")) {
                static bool articulationDetails;
                ImGui::Checkbox("Details", &articulationDetails);

                std::vector<SJoint *> activeJoints;
                if (articulation->getType() == EArticulationType::DYNAMIC) {
                  auto a = static_cast<sapien::SArticulation *>(articulation);
                  auto js = a->getSJoints();
                  for (auto j : js) {
                    if (j->getDof()) {
                      activeJoints.push_back(j);
                    }
                  }
                }

                int i = 0;
                for (auto &joint : jointValues) {
                  ImGui::Text("joint: %s", joint.name.c_str());
                  if (ImGui::SliderFloat(("##" + std::to_string(i)).c_str(), &joint.value,
                                         std::max(joint.limits[0], -10.f),
                                         std::min(joint.limits[1], 10.f))) {
                    std::vector<PxReal> v;
                    for (auto j : jointValues) {
                      v.push_back(j.value);
                    }
                    articulation->setQpos(v);
                  }
                  if (articulationDetails && activeJoints.size()) {
                    auto j = activeJoints[i];
                    float friction = j->getFriction();
                    float stiffness = j->getDriveStiffness();
                    float damping = j->getDriveDamping();
                    float maxForce = j->getDriveForceLimit();
                    float target = j->getDriveTarget();
                    float vtarget = j->getDriveVelocityTarget();
                    ImGui::Text("Friction: %.2f", friction);
                    ImGui::Text("Damping: %.2f", damping);
                    ImGui::Text("Stiffness: %.2f", stiffness);
                    if (maxForce > 1e6) {
                      ImGui::Text("Max Force: >1e6");
                    } else {
                      ImGui::Text("Max Force: %.2f", maxForce);
                    }
                    if (stiffness > 0) {
                      ImGui::Text("Drive Position Target: %.2f", target);
                      ImGui::Text("Drive Velocity Target: %.2f", vtarget);
                    }
                    ImGui::NewLine();
                  }
                  ++i;
                }
                ImGui::TreePop();
              }

              // show links
              if (ImGui::TreeNode("Link Tree")) {
                auto links = articulation->getBaseLinks();
                auto joints = articulation->getBaseJoints();

                struct LinkNode {
                  uint32_t parent;
                  uint32_t index;
                  std::vector<uint32_t> children;
                };
                std::vector<LinkNode> nodes(links.size());
                uint32_t root = joints.size();
                for (uint32_t i = 0; i < joints.size(); ++i) {
                  auto p = joints[i]->getParentLink();
                  nodes[i].index = i;
                  if (p) {
                    nodes[i].parent = p->getIndex();
                    nodes[p->getIndex()].children.push_back(i);
                  } else {
                    root = i;
                  }
                }

                std::vector<uint32_t> stack;
                std::vector<uint32_t> indents;
                stack.push_back(root);
                indents.push_back(0);

                while (!stack.empty()) {
                  uint32_t idx = stack.back();
                  uint32_t indent = indents.back();
                  stack.pop_back();
                  indents.pop_back();

                  if (links[idx] == mCurrentSelection) {
                    ImGui::TextColored({1, 0, 0, 1}, "%sLink %i: %s",
                                       std::string(indent, ' ').c_str(), idx,
                                       links[idx]->getName().c_str());
                  } else {
                    if (ImGui::Selectable((std::string(indent, ' ') + "Link " +
                                           std::to_string(idx) + ": " + links[idx]->getName())
                                              .c_str())) {

                      select(links[idx]);
                    }
                  }
                  for (uint32_t c : nodes[idx].children) {
                    stack.push_back(c);
                    indents.push_back(indent + 2);
                  }
                }
                ImGui::TreePop();
              }
            }
          }
        }
        ImGui::End();
      }

      {
        auto err = glGetError();
        if (err != GL_NO_ERROR) {
          spdlog::get("SAPIEN")->error("Error0 {:x}", err);
          throw "";
        }
      }

      ImGui::Render();
      ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
      {
        auto err = glGetError();
        if (err != GL_NO_ERROR) {
          spdlog::get("SAPIEN")->error("Error1 {:x}", err);
          throw "";
        }
      }
    }

    mRenderer->mContext->swapBuffers();

    if (changeShader) {
      if (renderMode == ALBEDO) {
        mRenderer->mContext->renderer.setDisplayShader(
            mRenderer->mGlslDir + "/display.vsh", mRenderer->mGlslDir + "/display_albedo.fsh");
      } else if (renderMode == NORMAL) {
        mRenderer->mContext->renderer.setDisplayShader(
            mRenderer->mGlslDir + "/display.vsh", mRenderer->mGlslDir + "/display_normal.fsh");
      } else if (renderMode == DEPTH) {
        mRenderer->mContext->renderer.setDisplayShader(mRenderer->mGlslDir + "/display.vsh",
                                                       mRenderer->mGlslDir + "/display_depth.fsh");
      }
    }
  } while (paused);
}

void OptifuserController::onEvent(EventActorPreDestroy &e) {
  if (e.actor == mCurrentFocus) {
    focus(nullptr);
  }
  if (e.actor == mCurrentSelection) {
    mCurrentSelection = nullptr;
  }
}

} // namespace Renderer
} // namespace sapien

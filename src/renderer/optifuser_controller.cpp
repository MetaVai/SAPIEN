#include "optifuser_controller.h"
#include "articulation/sapien_articulation.h"
#include "articulation/sapien_joint.h"
#include "articulation/sapien_link.h"
#include "render_interface.h"
#include "sapien_actor.h"
#include "sapien_scene.h"
#include "simulation.h"
#include <spdlog/spdlog.h>
#define IMGUI_IMPL_OPENGL_LOADER_GLEW
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

constexpr int WINDOW_WIDTH = 1200, WINDOW_HEIGHT = 800;

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

OptifuserController::OptifuserController(OptifuserRenderer *renderer) : mRenderer(renderer) {
  mCamera.setUp({0, 0, 1});
  mCamera.setForward({1, 0, 0});
  mCamera.position = {0, 0, 1};
  mCamera.rotateYawPitch(0, 0);
  mCamera.fovy = glm::radians(45.f);
  mCamera.aspect = WINDOW_WIDTH / (float)WINDOW_HEIGHT;
}
void OptifuserController::showWindow() { mRenderer->mContext->showWindow(); }
void OptifuserController::hideWindow() { mRenderer->mContext->hideWindow(); }

void OptifuserController::setCurrentScene(SScene *scene) { mScene = scene; }

bool OptifuserController::shouldQuit() { return mShouldQuit; }

void OptifuserController::render() {

#ifdef _USE_OPTIX
  static Optifuser::OptixRenderer *pathTracer = nullptr;
#endif

  static int renderMode = 0;
  static float moveSpeed = 3.f;
  mRenderer->mContext->processEvents();
  float framerate = ImGui::GetIO().Framerate;

  float dt = 1.f * moveSpeed / framerate;
  if (Optifuser::getInput().getKeyState(GLFW_KEY_W)) {
    mCamera.moveForwardRight(dt, 0);
#ifdef _USE_OPTIX
    if (renderMode == PATHTRACER) {
      pathTracer->invalidateCamera();
    }
#endif
  } else if (Optifuser::getInput().getKeyState(GLFW_KEY_S)) {
    mCamera.moveForwardRight(-dt, 0);
#ifdef _USE_OPTIX
    if (renderMode == PATHTRACER) {
      pathTracer->invalidateCamera();
    }
#endif
  } else if (Optifuser::getInput().getKeyState(GLFW_KEY_A)) {
    mCamera.moveForwardRight(0, -dt);
#ifdef _USE_OPTIX
    if (renderMode == PATHTRACER) {
      pathTracer->invalidateCamera();
    }
#endif
  } else if (Optifuser::getInput().getKeyState(GLFW_KEY_D)) {
    mCamera.moveForwardRight(0, dt);
#ifdef _USE_OPTIX
    if (renderMode == PATHTRACER) {
      pathTracer->invalidateCamera();
    }
#endif
  }

  if (Optifuser::getInput().getKeyDown(GLFW_KEY_Q)) {
    mShouldQuit = true;
  }

  mCamera.aspect = static_cast<float>(mRenderer->mContext->getWidth()) /
                   static_cast<float>(mRenderer->mContext->getHeight());

  static bool renderGui = true;
  if (Optifuser::getInput().getKeyDown(GLFW_KEY_E)) {
    renderGui = !renderGui;
  }
  if (Optifuser::getInput().getMouseButton(GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS) {
    double dx, dy;
    Optifuser::getInput().getCursorDelta(dx, dy);
    mCamera.rotateYawPitch(-dx / 1000.f, -dy / 1000.f);
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
    mRenderer->mContext->renderer.renderScene(*currentScene->getScene(), mCamera);

    if (renderMode == SEGMENTATION) {
      mRenderer->mContext->renderer.displaySegmentation();
    } else if (renderMode == CUSTOM) {
      mRenderer->mContext->renderer.displayUserTexture();
#ifdef _USE_OPTIX
    } else if (renderMode == PATHTRACER) {
      // path tracer
      pathTracer->numRays = 4;
      pathTracer->max_iterations = 100000;
      pathTracer->renderScene(*currentScene->getScene(), mCamera);
      pathTracer->display();
#endif
    } else {
      mRenderer->mContext->renderer.displayLighting();
    }
  }

  // static int pickedId = -1, pickedRenderId = -1;

  if (Optifuser::getInput().getMouseDown(GLFW_MOUSE_BUTTON_LEFT)) {
    int x, y;
    Optifuser::getInput().getCursor(x, y);
    int pickedId = mRenderer->mContext->renderer.pickSegmentationId(x, y);
    mGuiModel.linkId = pickedId;

    int pickedRenderId = 0;
    if (pickedId) {
      pickedRenderId = mRenderer->mContext->renderer.pickObjectId(x, y);
    }
  }

  if (mGuiModel.linkId) {
    SActorBase *actor = mScene->findActorById(mGuiModel.linkId);
    SLinkBase *link = mScene->findArticulationLinkById(mGuiModel.linkId);
    if (actor) {
      mGuiModel.linkModel.name = actor->getName();
      mGuiModel.linkModel.transform = actor->getPxActor()->getGlobalPose();
      mGuiModel.linkModel.col1 = actor->getCollisionGroup1();
      mGuiModel.linkModel.col2 = actor->getCollisionGroup2();
      mGuiModel.linkModel.col3 = actor->getCollisionGroup3();
      mGuiModel.articulationId = 0;
    } else if (link) {
      mGuiModel.linkModel.name = link->getName();
      mGuiModel.linkModel.transform = link->getPxActor()->getGlobalPose();
      mGuiModel.linkModel.col1 = link->getCollisionGroup1();
      mGuiModel.linkModel.col2 = link->getCollisionGroup2();
      mGuiModel.linkModel.col3 = link->getCollisionGroup3();
      auto articulation = link->getArticulation();
      mGuiModel.articulationId = 1;
      mGuiModel.articulationModel.name = articulation->getName();

      mGuiModel.articulationModel.jointModel.resize(articulation->dof());
      uint32_t n = 0;
      auto qpos = articulation->getQpos();
      for (auto j : articulation->getBaseJoints()) {
        auto limits = j->getLimits();
        for (uint32_t i = 0; i < j->getDof(); ++i) {
          mGuiModel.articulationModel.jointModel[n].name = j->getName();
          mGuiModel.articulationModel.jointModel[n].limits = limits[i];
          mGuiModel.articulationModel.jointModel[n].value = qpos[n];
          ++n;
        }
      }

    } else {
      spdlog::error(
          "User picked an unregistered object. There is probably some implementation error!");
    }

    auto &pos = mGuiModel.linkModel.transform.p;
    auto &quat = mGuiModel.linkModel.transform.q;
    currentScene->getScene()->clearAxes();
    currentScene->getScene()->addAxes({pos.x, pos.y, pos.z}, {quat.w, quat.x, quat.y, quat.z});
  }

  static const uint32_t imguiWindowSize = 300;
  static int camIndex = -1;
  if (renderGui) {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    // ImGui::Begin("Save##window");
    // {
    //   if (ImGui::CollapsingHeader("Save")) {
    //     static char buf[1000];
    //     ImGui::InputText("##input_buffer", buf, 1000);
    //     if (ImGui::Button("Save##button")) {
    //       std::cout << "save called" << std::endl;
    //       // saveCallback(saveNames.size(), std::string(buf));
    //     }
    //   }
    //   if (ImGui::CollapsingHeader("Load")) {
    //     for (uint32_t i = 0; i < saveNames.size(); ++i) {
    //       ImGui::Text("%s", saveNames[i].c_str());
    //       ImGui::SameLine(100);
    //       if (ImGui::Button(("load##" + std::to_string(i)).c_str())) {
    //         // saveActionCallback(i, 0);
    //       }
    //       ImGui::SameLine(150);
    //       if (ImGui::Button(("delete##" + std::to_string(i)).c_str())) {
    //         // saveActionCallback(i, 1);
    //       }
    //     }
    //   }
    // }
    // ImGui::End();

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(imguiWindowSize, mRenderer->mContext->getHeight()));

    ImGui::Begin("Render Options");
    {
      if (ImGui::CollapsingHeader("Render Mode", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::RadioButton("Lighting", &renderMode, RenderMode::LIGHTING)) {
          mRenderer->mContext->renderer.setDeferredShader(mRenderer->mGlslDir + "/deferred.vsh",
                                                          mRenderer->mGlslDir + "/deferred.fsh");
        };
        if (ImGui::RadioButton("Albedo", &renderMode, RenderMode::ALBEDO)) {
          mRenderer->mContext->renderer.setDeferredShader(
              mRenderer->mGlslDir + "/deferred.vsh", mRenderer->mGlslDir + "/deferred_albedo.fsh");
        }
        if (ImGui::RadioButton("Normal", &renderMode, RenderMode::NORMAL)) {
          mRenderer->mContext->renderer.setDeferredShader(
              mRenderer->mGlslDir + "/deferred.vsh", mRenderer->mGlslDir + "/deferred_normal.fsh");
        }
        if (ImGui::RadioButton("Depth", &renderMode, RenderMode::DEPTH)) {
          mRenderer->mContext->renderer.setDeferredShader(
              mRenderer->mGlslDir + "/deferred.vsh", mRenderer->mGlslDir + "/deferred_depth.fsh");
        }
        if (ImGui::RadioButton("Segmentation", &renderMode, RenderMode::SEGMENTATION)) {
          mRenderer->mContext->renderer.setGBufferShader(mRenderer->mGlslDir + "/gbuffer.vsh",
                                                         mRenderer->mGlslDir +
                                                             "/gbuffer_segmentation.fsh");
        }
        if (ImGui::RadioButton("Custom", &renderMode, RenderMode::CUSTOM)) {
          mRenderer->mContext->renderer.setGBufferShader(mRenderer->mGlslDir + "/gbuffer.vsh",
                                                         mRenderer->mGlslDir +
                                                             "/gbuffer_segmentation.fsh");
        }
#ifdef _USE_OPTIX
        if (ImGui::RadioButton("PathTracer", &renderMode, RenderMode::PATHTRACER)) {
          if (pathTracer) {
            delete pathTracer;
          }
          pathTracer = new Optifuser::OptixRenderer();
          pathTracer->setBlackBackground();
          pathTracer->init(mRenderer->mContext->getWidth(), mRenderer->mContext->getHeight());
        } else {
        }
#endif
      }

#ifdef _USE_OPTIX
      if (renderMode == PATHTRACER) {
        glEnable(GL_FRAMEBUFFER_SRGB);
      } else {
        glDisable(GL_FRAMEBUFFER_SRGB);
      }
#endif

      if (ImGui::CollapsingHeader("Main Camera", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Position");
        ImGui::Text("%-4.3f %-4.3f %-4.3f", mCamera.position.x, mCamera.position.y,
                    mCamera.position.z);
        ImGui::Text("Forward");
        auto forward = mCamera.getRotation() * glm::vec3(0, 0, -1);
        ImGui::Text("%-4.3f %-4.3f %-4.3f", forward.x, forward.y, forward.z);
        ImGui::Text("Fov");
        ImGui::SliderAngle("##fov(y)", &mCamera.fovy, 1.f, 90.f);
        ImGui::Text("Move speed");
        ImGui::SliderFloat("##speed", &moveSpeed, 1.f, 10.f);
        ImGui::Text("Width: %d", mRenderer->mContext->getWidth());
        ImGui::SameLine();
        ImGui::Text("Height: %d", mRenderer->mContext->getHeight());
        ImGui::SameLine();
        ImGui::Text("Aspect: %.2f", mCamera.aspect);
        // ImGui::Text("Picked link id: %d", pickedId);
        // ImGui::Text("Picked render id: %d", pickedRenderId);
      }

      if (ImGui::CollapsingHeader("Mounted Cameras")) {
        ImGui::RadioButton("None##camera", &camIndex, -1);

        if (currentScene) {
          auto cameras = currentScene->getCameras();
          for (uint32_t i = 0; i < cameras.size(); ++i) {
            ImGui::RadioButton((cameras[i]->getName() + "##camera" + std::to_string(i)).c_str(),
                               &camIndex, i);
          }

          if (camIndex >= 0) {
            uint32_t width = cameras[camIndex]->getWidth();
            uint32_t height = cameras[camIndex]->getHeight();
            cameras[camIndex]->takePicture();
            ImGui::Image(
                reinterpret_cast<ImTextureID>(static_cast<OptifuserCamera *>(cameras[camIndex])
                                                  ->mRenderContext->renderer.outputtex),
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

    if (mGuiModel.linkId) {
      ImGui::SetNextWindowPos(ImVec2(mRenderer->mContext->getWidth() - imguiWindowSize, 0));
      ImGui::SetNextWindowSize(ImVec2(imguiWindowSize, mRenderer->mContext->getHeight()));
      ImGui::Begin("Selected Object");
      {
        if (ImGui::CollapsingHeader("Actor", ImGuiTreeNodeFlags_DefaultOpen)) {
          ImGui::Text("name: %s", mGuiModel.linkModel.name.c_str());
          ImGui::Text("col1: #%08x, col2: #%08x", mGuiModel.linkModel.col1,
                      mGuiModel.linkModel.col2);
          ImGui::Text("col3: #%08x", mGuiModel.linkModel.col3);
        }
      }
      if (mGuiModel.articulationId) {
        if (ImGui::CollapsingHeader("Articulation", ImGuiTreeNodeFlags_DefaultOpen)) {
          ImGui::Text("name: %s", mGuiModel.articulationModel.name.c_str());
          ImGui::Text(" dof: %ld", mGuiModel.articulationModel.jointModel.size());

          int i = 0;
          for (auto &joint : mGuiModel.articulationModel.jointModel) {
            ImGui::Text("%s", joint.name.c_str());
            if (ImGui::SliderFloat(("##" + std::to_string(++i)).c_str(), &joint.value,
                                   std::max(joint.limits[0], -10.f),
                                   std::min(joint.limits[1], 10.f))) {
              std::vector<PxReal> v;
              SLinkBase *link = mScene->findArticulationLinkById(mGuiModel.linkId);
              auto articulation = link->getArticulation();
              for (auto j : mGuiModel.articulationModel.jointModel) {
                v.push_back(j.value);
              }
              articulation->setQpos(v);
            }
          }
        }
      }
      ImGui::End();
    }

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
  }
  mRenderer->mContext->swapBuffers();
}
} // namespace Renderer
} // namespace sapien
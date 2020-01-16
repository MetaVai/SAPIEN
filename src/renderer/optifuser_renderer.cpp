#include "optifuser_renderer.h"
#include <objectLoader.h>
#include <spdlog/spdlog.h>
#define IMGUI_IMPL_OPENGL_LOADER_GLEW
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

namespace sapien {
namespace Renderer {

static std::vector<std::string> saveNames = {};

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

constexpr int WINDOW_WIDTH = 1200, WINDOW_HEIGHT = 800;

//======== Begin Rigidbody ========//

OptifuserRigidbody::OptifuserRigidbody(OptifuserScene *scene,
                                       std::vector<Optifuser::Object *> const &objects)
    : mParentScene(scene), mObjects(objects) {}

void OptifuserRigidbody::setUniqueId(uint32_t uniqueId) {
  mUniqueId = uniqueId;
  for (auto obj : mObjects) {
    obj->setObjId(uniqueId);
  }
}

uint32_t OptifuserRigidbody::getUniqueId() const { return mUniqueId; }

void OptifuserRigidbody::setSegmentationId(uint32_t segmentationId) {
  mSegmentationId = segmentationId;
  for (auto obj : mObjects) {
    obj->setSegmentId(segmentationId);
  }
}

uint32_t OptifuserRigidbody::getSegmentationId() const { return mSegmentationId; }

void OptifuserRigidbody::setSegmentationCustomData(const std::vector<float> &customData) {
  for (auto obj : mObjects) {
    obj->setUserData(customData);
  }
}

void OptifuserRigidbody::setInitialPose(const physx::PxTransform &transform) {
  mInitialPose = transform;
  update({{0, 0, 0}, physx::PxIdentity});
}

void OptifuserRigidbody::update(const physx::PxTransform &transform) {
  auto pose = transform * mInitialPose;
  for (auto obj : mObjects) {
    obj->position = {pose.p.x, pose.p.y, pose.p.z};
    obj->setRotation({pose.q.w, pose.q.x, pose.q.y, pose.q.z});
  }
}

void OptifuserRigidbody::destroy() { mParentScene->removeRigidbody(this); }

//======== End Rigidbody ========//

//======== Begin Scene ========//
OptifuserScene::OptifuserScene(OptifuserRenderer *renderer, std::string const &name)
    : mParentRenderer(renderer), mScene(std::make_unique<Optifuser::Scene>()), mName(name) {}

Optifuser::Scene *OptifuserScene::getScene() { return mScene.get(); }

IPxrRigidbody *OptifuserScene::addRigidbody(const std::string &meshFile,
                                            const physx::PxVec3 &scale) {
  auto objects = Optifuser::LoadObj(meshFile);
  std::vector<Optifuser::Object *> objs;
  if (objects.empty()) {
    spdlog::error("Failed to load damaged file: {}", meshFile);
    return nullptr;
  }
  for (auto &obj : objects) {
    obj->scale = {scale.x, scale.y, scale.z};
    objs.push_back(obj.get());
    mScene->addObject(std::move(obj));
  }

  mBodies.push_back(std::make_unique<OptifuserRigidbody>(this, objs));

  return mBodies.back().get();
}

IPxrRigidbody *OptifuserScene::addRigidbody(physx::PxGeometryType::Enum type,
                                            const physx::PxVec3 &scale,
                                            const physx::PxVec3 &color) {
  std::unique_ptr<Optifuser::Object> obj;
  switch (type) {
  case physx::PxGeometryType::eBOX: {
    obj = Optifuser::NewFlatCube();
    obj->scale = {scale.x, scale.y, scale.z};
    break;
  }
  case physx::PxGeometryType::eSPHERE: {
    obj = Optifuser::NewSphere();
    obj->scale = {scale.x, scale.y, scale.z};
    break;
  }
  case physx::PxGeometryType::ePLANE: {
    obj = Optifuser::NewYZPlane();
    obj->scale = {scale.x, scale.y, scale.z};
    break;
  }
  case physx::PxGeometryType::eCAPSULE: {
    obj = Optifuser::NewCapsule(scale.x, scale.y);
    obj->scale = {1, 1, 1};
    break;
  }
  default:
    spdlog::error("Failed to add Rididbody: unimplemented shape");
    return nullptr;
  }

  obj->material.kd = {color.x, color.y, color.z, 1};

  mBodies.push_back(std::make_unique<OptifuserRigidbody>(this, std::vector{obj.get()}));
  mScene->addObject(std::move(obj));

  return mBodies.back().get();
}

void OptifuserScene::removeRigidbody(IPxrRigidbody *body) {
  auto it = mBodies.begin();
  for (; it != mBodies.end(); ++it) {
    if (it->get() == body) {
      mBodies.erase(it);
      return;
    }
  }
}

ICamera *OptifuserScene::addCamera(std::string const &name, uint32_t width, uint32_t height,
                                   float fovx, float fovy, float near, float far,
                                   std::string const &shaderDir) {
  spdlog::warn("Note: current camera implementation does not support non-square pixels, and fovy "
               "will take precedence.");
  auto cam = std::make_unique<OptifuserCamera>(name, width, height, fovy, this, shaderDir);
  cam->near = near;
  cam->far = far;
  mCameras.push_back(std::move(cam));
  return mCameras.back().get();
}

void OptifuserScene::removeCamera(ICamera *camera) {
  std::remove_if(mCameras.begin(), mCameras.end(),
                 [camera](auto &c) { return camera == c.get(); });
}

std::vector<ICamera *> OptifuserScene::getCameras() {
  std::vector<ICamera *> cams;
  for (auto &cam : mCameras) {
    cams.push_back(cam.get());
  }
  return cams;
}

void OptifuserScene::destroy() { mParentRenderer->removeScene(this); }

void OptifuserScene::setAmbientLight(std::array<float, 3> const &color) {
  mScene->setAmbientLight({color[0], color[1], color[2]});
}

void OptifuserScene::setShadowLight(std::array<float, 3> const &direction,
                                    std::array<float, 3> const &color) {
  mScene->setShadowLight(
      {{direction[0], direction[1], direction[2]}, {color[0], color[1], color[2]}});
}

void OptifuserScene::addPointLight(std::array<float, 3> const &position,
                                   std::array<float, 3> const &color) {
  mScene->addPointLight({{position[0], position[1], position[2]}, {color[0], color[1], color[2]}});
}

void OptifuserScene::addDirectionalLight(std::array<float, 3> const &direction,
                                         std::array<float, 3> const &color) {
  mScene->addDirectionalLight(
      {{direction[0], direction[1], direction[2]}, {color[0], color[1], color[2]}});
}

//======== End Scene ========//

//======== Begin Renderer ========//

OptifuserRenderer::OptifuserRenderer(const std::string &glslDir, const std::string &glslVersion)
    : mGlslDir(glslDir) {
  mContext = &Optifuser::GLFWRenderContext::Get(WINDOW_WIDTH, WINDOW_HEIGHT);
  mContext->initGui(glslVersion);

  mContext->renderer.setShadowShader(glslDir + "/shadow.vsh", glslDir + "/shadow.fsh");
  mContext->renderer.setGBufferShader(glslDir + "/gbuffer.vsh",
                                      glslDir + "/gbuffer_segmentation.fsh");
  mContext->renderer.setDeferredShader(glslDir + "/deferred.vsh", glslDir + "/deferred.fsh");
  mContext->renderer.setAxisShader(glslDir + "/axes.vsh", glslDir + "/axes.fsh");
  mContext->renderer.enablePicking();
  mContext->renderer.enableAxisPass();
}

IPxrScene *OptifuserRenderer::createScene(std::string const &name) {
  mScenes.push_back(std::make_unique<OptifuserScene>(this, name));
  return mScenes.back().get();
}

void OptifuserRenderer::removeScene(IPxrScene *scene) {
  std::remove_if(mScenes.begin(), mScenes.end(), [scene](auto &s) { return scene == s.get(); });
}

//======== End Renderer ========//

} // namespace Renderer
} // namespace sapien

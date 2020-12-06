#pragma once
#include "event_system/event_system.h"
#include "id_generator.h"
#include "sapien_contact.h"
#include "sapien_trigger.h"
#include "sapien_shape.h"
#include <PxPhysicsAPI.h>
#include <functional>
#include <string>
#include <vector>

namespace sapien {
using namespace physx;

namespace Renderer {
class IPxrRigidbody;
};

class SDrive;
class SScene;

enum class EActorType {
  STATIC,
  KINEMATIC,
  DYNAMIC,
  ARTICULATION_LINK,
  KINEMATIC_ARTICULATION_LINK
};

using StepCallback = std::function<void(SActorBase *actor, float timestep)>;
using ContactCallback =
    std::function<void(SActorBase *actor, SActorBase *other, SContact const &contact)>;
using TriggerCallback =
    std::function<void(SActorBase *triggerActor, SActorBase *otherActor, STrigger const &trigger)>;

class SActorBase : public EventEmitter<EventActorPreDestroy>, public EventEmitter<EventActorStep> {
protected:
  std::string mName{""};
  physx_id_t mId{0};
  SScene *mParentScene{};
  std::vector<Renderer::IPxrRigidbody *> mRenderBodies{};
  std::vector<Renderer::IPxrRigidbody *> mCollisionBodies{};

  std::vector<SDrive *> mDrives{};

  uint32_t mCol1{0};
  uint32_t mCol2{0};
  uint32_t mCol3{0};

  bool collisionRender{false};
  bool mHidden{false};
  float mDisplayVisibility{1.f};

  int mDestroyedState{0};

  std::vector<StepCallback> mOnStepCallback;
  std::vector<ContactCallback> mOnContactCallback;
  std::vector<TriggerCallback> mOnTriggerCallback;

public:
  void renderCollisionBodies(bool collision);
  bool isRenderingCollision() const;

  void hideVisual();
  void unhideVisual();
  bool isHidingVisual() const;

  inline std::vector<SDrive *> getDrives() const { return mDrives; }

  // should not be called by users
  void addDrive(SDrive *drive);

  // should not be called by users
  void removeDrive(SDrive *drive);

  inline std::string getName() { return mName; };
  inline void setName(const std::string &name) { mName = name; }
  inline physx_id_t getId() { return mId; }
  inline SScene *getScene() { return mParentScene; }

  PxTransform getPose();
  inline uint32_t getCollisionGroup1() { return mCol1; }
  inline uint32_t getCollisionGroup2() { return mCol2; }
  inline uint32_t getCollisionGroup3() { return mCol3; }

  std::vector<std::unique_ptr<SShape>> getCollisionShapes();

  // render
  std::vector<Renderer::IPxrRigidbody *> getRenderBodies();
  std::vector<Renderer::IPxrRigidbody *> getCollisionBodies();
  void updateRender(PxTransform const &pose);

  virtual PxRigidActor *getPxActor() = 0;
  virtual EActorType getType() const = 0;
  virtual ~SActorBase() = default;

  // called by scene to notify a simulation step is about to happen
  virtual void prestep();

  void setDisplayVisibility(float visibility);
  float getDisplayVisibility() const;

  /** internal use only, actors marked as destroyed will be removed in the next step */
  inline void markDestroyed() {
    if (mDestroyedState == 0) {
      mDestroyedState = 1;
    }
  }
  /** check whether the object is in the process of being destroyed */
  inline bool isBeingDestroyed() const { return mDestroyedState != 0; }

  /** internal use only, destroy has several stages, set the stage */
  inline void setDestroyedState(int state) { mDestroyedState = state; }
  /** internal use only, destroy has several stages, check which stage it is in */
  inline int getDestroyedState() const { return mDestroyedState; }

  inline virtual std::vector<PxReal> packData() { return {}; };
  inline virtual void unpackData(std::vector<PxReal> const &data){};

  // callback from python
  inline void unregisterOnContact() { mOnContactCallback = {}; }
  inline void onContact(ContactCallback callback) { mOnContactCallback = {callback}; }
  inline void unregisterOnStep() { mOnStepCallback = {}; }
  inline void onStep(StepCallback callback) { mOnStepCallback = {callback}; }
  inline void unregisterOnTrigger() { mOnTriggerCallback = {}; }
  inline void onTrigger(TriggerCallback callback) { mOnTriggerCallback = {callback}; }

  void notifyContact(SActorBase &other, SContact const &contact);
  void notifyTrigger(SActorBase &other, STrigger const &trigger);
  void notifyStep();

protected:
  SActorBase(physx_id_t id, SScene *scene, std::vector<Renderer::IPxrRigidbody *> renderBodies,
             std::vector<Renderer::IPxrRigidbody *> collisionBodies);
};

class SActorDynamicBase : public SActorBase {

public:
  PxVec3 getVelocity();
  PxVec3 getAngularVelocity();
  PxReal getMass();
  PxVec3 getInertia();
  PxTransform getCMassLocalPose();

  PxRigidBody *getPxActor() override = 0;
  virtual void addForceAtPoint(PxVec3 const &force, PxVec3 const &pos);
  virtual void addForceTorque(PxVec3 const &force, PxVec3 const &torque);
  void setDamping(PxReal linear, PxReal angular);

protected:
  using SActorBase::SActorBase;
};

} // namespace sapien

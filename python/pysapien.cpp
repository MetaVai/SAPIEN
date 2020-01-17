#include <pybind11/numpy.h>
#include <pybind11/operators.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "actor_builder.h"
#include "renderer/render_interface.h"
#include "sapien_actor.h"
#include "sapien_actor_base.h"
#include "sapien_contact.h"
#include "sapien_scene.h"
#include "simulation.h"

#include "articulation/articulation_builder.h"
#include "articulation/sapien_articulation.h"
#include "articulation/sapien_articulation_base.h"
#include "articulation/sapien_joint.h"
#include "articulation/sapien_link.h"
#include "articulation/urdf_loader.h"

#include "renderer/optifuser_controller.h"
#include "renderer/optifuser_renderer.h"

using namespace sapien;
namespace py = pybind11;

PxVec3 array2vec3(const py::array_t<float> &arr) { return {arr.at(0), arr.at(1), arr.at(2)}; }

template <typename T> py::array_t<T> make_array(std::vector<T> const &values) {
  return py::array_t(values.size(), values.data());
}

py::array_t<PxReal> vec32array(PxVec3 const &vec) {
  std::vector<PxReal> v = {vec.x, vec.y, vec.z};
  return make_array(v);
}

PYBIND11_MODULE(pysapien, m) {

  //======== Internal ========//
  py::enum_<PxSolverType::Enum>(m, "SolverType")
      .value("PGS", PxSolverType::ePGS)
      .value("TGS", PxSolverType::eTGS)
      .export_values();

  py::enum_<PxArticulationJointType::Enum>(m, "ArticulationJointType")
      .value("PRISMATIC", PxArticulationJointType::ePRISMATIC)
      .value("REVOLUTE", PxArticulationJointType::eREVOLUTE)
      .value("SPHERICAL", PxArticulationJointType::eSPHERICAL)
      .value("FIX", PxArticulationJointType::eFIX)
      .value("UNDEFINED", PxArticulationJointType::eUNDEFINED)
      .export_values();

  py::class_<PxMaterial, std::unique_ptr<PxMaterial, py::nodelete>>(m, "PxMaterial")
      .def("get_static_friction", &PxMaterial::getStaticFriction)
      .def("get_dynamic_friction", &PxMaterial::getDynamicFriction)
      .def("get_restitution", &PxMaterial::getRestitution)
      .def("set_static_friction", &PxMaterial::setStaticFriction)
      .def("set_dynamic_friction", &PxMaterial::setDynamicFriction)
      .def("set_restitution", &PxMaterial::setRestitution);

  py::class_<PxTransform>(m, "Pose")
      .def(py::init([](py::array_t<float> p, py::array_t<float> q) {
             return new PxTransform({p.at(0), p.at(1), p.at(2)},
                                    {q.at(1), q.at(2), q.at(3), q.at(0)});
           }),
           py::return_value_policy::automatic, py::arg("p") = make_array<float>({0, 0, 0}),
           py::arg("q") = make_array<float>({1, 0, 0, 0}))
      .def_property_readonly(
          "p", [](PxTransform &t) { return py::array_t<PxReal>(3, (PxReal *)(&t.p)); })
      .def_property_readonly("q",
                             [](PxTransform &t) {
                               return make_array<float>({t.q.w, t.q.x, t.q.y, t.q.z});
                             })
      .def("inv", &PxTransform::getInverse)
      .def("__repr__",
           [](const PxTransform &pose) {
             std::ostringstream oss;
             oss << "Position: x: " << pose.p.x << ", y: " << pose.p.y << ", z: " << pose.p.z
                 << "\n";
             oss << "Quaternion: w: " << pose.q.w << ", x: " << pose.q.x << ", y: " << pose.q.y
                 << ", z: " << pose.q.z << "\n";
             std::string repr = oss.str();
             return repr;
           })
      .def("transform", [](PxTransform &t, PxTransform &src) { return t.transform(src); })
      .def("set_p", [](PxTransform &t, const py::array_t<PxReal> &arr) { t.p = array2vec3(arr); })
      .def("set_q",
           [](PxTransform &t, const py::array_t<PxReal> &arr) {
             t.q = {arr.at(1), arr.at(2), arr.at(3), arr.at(0)}; // NOTE: wxyz to xyzw
           })
      .def(py::self * py::self);

  //======== Render Interface ========//
  py::class_<Renderer::IPxrRenderer>(m, "IPxrRenderer");

  py::class_<Renderer::ISensor>(m, "ISensor")
      .def("setInitialPose", &Renderer::ISensor::setInitialPose)
      .def("getPose", &Renderer::ISensor::getPose)
      .def("setPose", &Renderer::ISensor::setPose);

  py::class_<Renderer::ICamera, Renderer::ISensor>(m, "ICamera")
      .def("get_name", &Renderer::ICamera::getName)
      .def("get_width", &Renderer::ICamera::getWidth)
      .def("get_height", &Renderer::ICamera::getHeight)
      .def("get_fovy", &Renderer::ICamera::getFovy)
      .def("take_picture", &Renderer::ICamera::takePicture)
      .def("get_color_rgba",
           [](Renderer::ICamera &cam) {
             return py::array_t<float>(
                 {static_cast<int>(cam.getHeight()), static_cast<int>(cam.getWidth()), 4},
                 cam.getColorRGBA().data());
           })
      .def("get_albedo_rgba",
           [](Renderer::ICamera &cam) {
             return py::array_t<float>(
                 {static_cast<int>(cam.getHeight()), static_cast<int>(cam.getWidth()), 4},
                 cam.getAlbedoRGBA().data());
           })
      .def("get_normal_rgba",
           [](Renderer::ICamera &cam) {
             return py::array_t<float>(
                 {static_cast<int>(cam.getHeight()), static_cast<int>(cam.getWidth()), 4},
                 cam.getNormalRGBA().data());
           })
      .def("get_depth",
           [](Renderer::ICamera &cam) {
             return py::array_t<float>(
                 {static_cast<int>(cam.getHeight()), static_cast<int>(cam.getWidth())},
                 cam.getDepth().data());
           })
      .def("get_segmentation",
           [](Renderer::ICamera &cam) {
             return py::array_t<int>(
                 {static_cast<int>(cam.getHeight()), static_cast<int>(cam.getWidth())},
                 cam.getSegmentation().data());
           })
      .def("get_obj_segmentation", [](Renderer::ICamera &cam) {
        return py::array_t<int>(
            {static_cast<int>(cam.getHeight()), static_cast<int>(cam.getWidth())},
            cam.getObjSegmentation().data());
      });

  py::class_<Renderer::OptifuserRenderer, Renderer::IPxrRenderer>(m, "OptifuserRenderer")
      .def(py::init<std::string const &, std::string const &>(),
           py::arg("glsl_dir") = "glsl_shader/130", py::arg("glsl_version") = "130");

  py::class_<Renderer::OptifuserController>(m, "OptifuserController")
      .def(py::init<Renderer::OptifuserRenderer *>())
      .def_readonly("camera", &Renderer::OptifuserController::mCamera)
      .def("show_window", &Renderer::OptifuserController::showWindow)
      .def("hide_window", &Renderer::OptifuserController::hideWindow)
      .def("set_current_scene", &Renderer::OptifuserController::setCurrentScene)
      .def("render", &Renderer::OptifuserController::render)
      .def_property_readonly("should_quit", &Renderer::OptifuserController::shouldQuit);

  py::class_<Optifuser::FPSCameraSpec>(m, "FPSCameraSpec")
      .def_readwrite("name", &Optifuser::CameraSpec::name)
      .def_property_readonly(
          "position",
          [](Optifuser::FPSCameraSpec &c) { return py::array_t<float>(3, (float *)(&c.position)); })
      .def("set_position",
           [](Optifuser::FPSCameraSpec &c, const py::array_t<float> &arr) {
             c.position = {arr.at(0), arr.at(1), arr.at(2)};
           })
      .def("update", &Optifuser::FPSCameraSpec::update)
      .def("is_sane", &Optifuser::FPSCameraSpec::isSane)
      .def("set_forward",
           [](Optifuser::FPSCameraSpec &c, const py::array_t<float> &dir) {
             c.setForward({dir.at(0), dir.at(1), dir.at(2)});
           })
      .def("set_up",
           [](Optifuser::FPSCameraSpec &c, const py::array_t<float> &dir) {
             c.setUp({dir.at(0), dir.at(1), dir.at(2)});
           })
      .def("rotate_yaw_pitch", &Optifuser::FPSCameraSpec::rotateYawPitch)
      .def("move_forward_right", &Optifuser::FPSCameraSpec::moveForwardRight)
      .def("get_rotation0", [](Optifuser::FPSCameraSpec &c) {
        glm::quat q = c.getRotation0();
        return make_array<float>({q.w, q.x, q.y, q.z});
      });

  //======== Simulation ========//
  py::class_<Simulation>(m, "Simulation")
      .def(py::init<>())
      .def("set_renderer", &Simulation::setRenderer)
      .def("get_renderer", &Simulation::getRenderer, py::return_value_policy::reference)
      .def("create_physical_material", &Simulation::createPhysicalMaterial,
           py::return_value_policy::reference)
      .def("create_scene",
           [](Simulation &sim, std::string const &name, py::array_t<PxReal> const &gravity,
              PxSolverType::Enum solverType, bool enableCCD, bool enablePCM) {
             PxSceneFlags flags = PxSceneFlags();
             if (enableCCD) {
               flags |= PxSceneFlag::eENABLE_CCD;
             }
             if (enablePCM) {
               flags |= PxSceneFlag::eENABLE_PCM;
             }
             return sim.createScene(name, array2vec3(gravity), solverType, flags);
           });

  py::class_<SScene>(m, "SScene")
      .def_property_readonly("name", &SScene::getName)
      .def("set_timestep", &SScene::setTimestep)
      .def("get_timestep", &SScene::getTimestep)
      .def_property("timestep", &SScene::getTimestep, &SScene::setTimestep)
      .def("create_actor_builder", &SScene::createActorBuilder)
      .def("create_articulation_builder", &SScene::createArticulationBuilder)
      .def("create_urdf_loader", &SScene::createURDFLoader)
      .def("remove_actor", &SScene::removeActor)
      .def("remove_articulation", &SScene::removeArticulation)
      .def("find_actor_by_id", &SScene::findActorById, py::return_value_policy::reference)
      .def("find_articulation_link_by_link_id", &SScene::findArticulationLinkById,
           py::return_value_policy::reference)

      .def("add_mounted_camera", &SScene::addMountedCamera, py::return_value_policy::reference)
      .def("remove_mounted_camera", &SScene::removeMountedCamera)

      .def("step", &SScene::step)
      .def("update_render", &SScene::updateRender)
      .def("add_ground", &SScene::addGround, py::arg("altitude"), py::arg("render") = true,
           py::arg("material") = nullptr)
      .def("get_contacts", &SScene::getContacts)

      .def("set_shadow_light",
           [](SScene &s, py::array_t<PxReal> const &direction, py::array_t<PxReal> const &color) {
             s.setShadowLight(array2vec3(direction), array2vec3(color));
           })
      .def("set_ambient_light",
           [](SScene &s, py::array_t<PxReal> const &color) {
             s.setAmbientLight(array2vec3(color));
           })
      .def("add_point_light",
           [](SScene &s, py::array_t<PxReal> const &position,
              py::array_t<PxReal> const &direction) {
             s.addPointLight(array2vec3(position), array2vec3(direction));
           })
      .def("add_directional_light",
           [](SScene &s, py::array_t<PxReal> const &direction, py::array_t<PxReal> const &color) {
             s.addDirectionalLight(array2vec3(direction), array2vec3(color));
           });

  //======== Actor ========//

  py::class_<SActorBase>(m, "SActorBase")
      .def_property("name", &SActorBase::getName, &SActorBase::setName)
      .def_property_readonly("id", &SActorBase::getId)
      .def("get_scene", &SActorBase::getScene, py::return_value_policy::reference)

      .def_property_readonly("pose", &SActorBase::getPose)
      .def_property_readonly("col1", &SActorBase::getCollisionGroup1)
      .def_property_readonly("col2", &SActorBase::getCollisionGroup2)
      .def_property_readonly("col3", &SActorBase::getCollisionGroup3)

      // TODO: check if it is okay to return a vector of REFERENCED raw pointers
      .def_property_readonly("render_bodies", &SActorBase::getRenderBodies);

  py::class_<SActorDynamicBase, SActorBase>(m, "SActorDynamicBase")
      .def_property_readonly("velocity",
                             [](SActorDynamicBase &a) { return vec32array(a.getVel()); })
      .def_property_readonly("angular_velocity",
                             [](SActorDynamicBase &a) { return vec32array(a.getAngularVel()); })
      .def("add_force_at_point", [](SActorDynamicBase &a, py::array_t<PxReal> const &force,
                                    py::array_t<PxReal> const &point) {
        a.addForceAtPoint(array2vec3(force), array2vec3(point));
      });

  py::class_<SActorStatic, SActorBase>(m, "SActorStatic").def("set_pose", &SActorStatic::setPose);

  py::class_<SActor, SActorDynamicBase>(m, "SActor").def("set_pose", &SActor::setPose);

  py::class_<SLinkBase, SActorDynamicBase>(m, "SLinkBase")
      .def("get_index", &SLinkBase::getIndex)
      .def("get_articulation", &SLinkBase::getArticulation, py::return_value_policy::reference);

  py::class_<SLink, SLinkBase>(m, "SLink")
      .def("get_articulation", &SLink::getArticulation, py::return_value_policy::reference);

  //======== End Actor ========//

  //======== Joint ========//
  py::class_<SJointBase>(m, "SJointBase")
      .def_property("name", &SJointBase::getName, &SJointBase::setName)
      .def("get_parent_link", &SJointBase::getParentLink, py::return_value_policy::reference)
      .def("get_child_link", &SJointBase::getChildLink, py::return_value_policy::reference)
      .def("get_dof", &SJointBase::getDof)
      .def("get_limits",
           [](SJointBase &j) {
             auto limits = j.getLimits();
             return py::array_t<PxReal>({(int)limits.size(), 2},
                                        {sizeof(std::array<PxReal, 2>), sizeof(PxReal)},
                                        reinterpret_cast<PxReal *>(limits.data()));
           })
      // TODO implement set limits
      ;

  py::class_<SJoint, SJointBase>(m, "SJoint")
      .def("set_friction", &SJoint::setFriction)
      .def("set_drive_property", &SJoint::setDriveProperty)
      .def("set_drive_velocity_target", [](SJoint &j, PxReal v) { j.setDriveVelocityTarget(v); })
      .def("set_drive_target", [](SJoint &j, PxReal p) { j.setDriveTarget(p); })
      // TODO wrapper for array-valued targets
      .def("get_global_pose", &SJoint::getGlobalPose);

  //======== End Joint ========//

  //======== Articulation ========//
  py::enum_<EArticulationType>(m, "ArticulationType")
      .value("DYNAMIC", EArticulationType::DYNAMIC)
      .value("KINEMATIC", EArticulationType::KINEMATIC)
      .export_values();

  py::class_<SArticulationBase>(m, "SArticulationBase")
      .def_property("name", &SArticulationBase::getName, &SArticulationBase::setName)
      .def("get_base_links", &SArticulationBase::getBaseLinks)
      .def("get_base_joints", &SArticulationBase::getBaseJoints)
      .def_property_readonly("type", &SArticulationBase::getType)
      .def_property_readonly("dof", &SArticulationBase::dof)
      .def("get_qpos",
           [](SArticulationBase &a) {
             auto qpos = a.getQpos();
             return py::array_t<PxReal>(qpos.size(), qpos.data());
           })
      .def("set_qpos",
           [](SArticulationBase &a, const py::array_t<float> &arr) {
             a.setQpos(std::vector<PxReal>(arr.data(), arr.data() + arr.size()));
           })

      .def("get_qvel",
           [](SArticulationBase &a) {
             auto qvel = a.getQvel();
             return py::array_t<PxReal>(qvel.size(), qvel.data());
           })
      .def("set_qvel",
           [](SArticulationBase &a, const py::array_t<float> &arr) {
             a.setQvel(std::vector<PxReal>(arr.data(), arr.data() + arr.size()));
           })
      .def("get_qacc",
           [](SArticulationBase &a) {
             auto qacc = a.getQacc();
             return py::array_t<PxReal>(qacc.size(), qacc.data());
           })
      .def("set_qacc",
           [](SArticulationBase &a, const py::array_t<float> &arr) {
             a.setQacc(std::vector<PxReal>(arr.data(), arr.data() + arr.size()));
           })
      .def("get_qf",
           [](SArticulationBase &a) {
             auto qf = a.getQf();
             return py::array_t<PxReal>(qf.size(), qf.data());
           })
      .def("set_qf",
           [](SArticulationBase &a, const py::array_t<float> &arr) {
             a.setQf(std::vector<PxReal>(arr.data(), arr.data() + arr.size()));
           })

      .def("get_qlimits",
           [](SArticulationBase &a) {
             std::vector<std::array<PxReal, 2>> limits = a.getQlimits();
             return py::array_t<PxReal>({(int)limits.size(), 2},
                                        {sizeof(std::array<PxReal, 2>), sizeof(PxReal)},
                                        reinterpret_cast<PxReal *>(limits.data()));
           })
      // TODO: set_qlimits
      ;

  py::class_<SArticulationDrivable, SArticulationBase>(m, "SArticulationDrivable")
      .def("get_drive_target",
           [](SArticulationDrivable &a) {
             auto target = a.getDriveTarget();
             return py::array_t<PxReal>(target.size(), target.data());
           })
      .def("set_drive_target", [](SArticulationDrivable &a, const py::array_t<float> &arr) {
        a.setDriveTarget(std::vector<PxReal>(arr.data(), arr.data() + arr.size()));
      });

  py::class_<SArticulation, SArticulationDrivable>(m, "SArticulation")
      .def("get_links", &SArticulation::getSLinks)
      .def("get_joints", &SArticulation::getSJoints);

  //======== End Articulation ========//

  py::class_<SContact>(m, "SContact")
      .def_property_readonly("actor1", [](SContact &contact) { return contact.actors[0]; },
                             py::return_value_policy::reference)
      .def_property_readonly("actor2", [](SContact &contact) { return contact.actors[1]; },
                             py::return_value_policy::reference)
      .def_property_readonly(
          "point",
          [](SContact &contact) {
            return make_array<PxReal>({contact.point.x, contact.point.y, contact.point.z});
          })
      .def_property_readonly(
          "normal",
          [](SContact &contact) {
            return make_array<PxReal>({contact.normal.x, contact.normal.y, contact.normal.z});
          })
      .def_property_readonly(
          "impulse",
          [](SContact &contact) {
            return make_array<PxReal>({contact.impulse.x, contact.impulse.y, contact.impulse.z});
          })
      .def_readonly("separation", &SContact::separation);

  //======== Builders ========

  py::class_<ActorBuilder>(m, "ActorBuilder")
      .def("add_convex_shape_from_file",
           [](ActorBuilder &a, std::string const &filename, PxTransform const &pose,
              py::array_t<PxReal> const &scale, PxMaterial *material, PxReal density) {
             a.addConvexShapeFromFile(filename, pose, array2vec3(scale), material, density);
           },
           py::arg("filename"), py::arg("pose") = PxTransform(PxIdentity),
           py::arg("scale") = make_array<PxReal>({1, 1, 1}), py::arg("material") = nullptr,
           py::arg("density") = 1000)
      .def("add_multiple_convex_shapes_from_file",
           [](ActorBuilder &a, std::string const &filename, PxTransform const &pose,
              py::array_t<PxReal> const &scale, PxMaterial *material, PxReal density) {
             a.addMultipleConvexShapesFromFile(filename, pose, array2vec3(scale), material,
                                               density);
           },
           py::arg("filename"), py::arg("pose") = PxTransform(PxIdentity),
           py::arg("scale") = make_array<PxReal>({1, 1, 1}), py::arg("material") = nullptr,
           py::arg("density") = 1000)
      .def("add_box_shape",
           [](ActorBuilder &a, PxTransform const &pose, py::array_t<PxReal> const &size,
              PxMaterial *material,
              PxReal density) { a.addBoxShape(pose, array2vec3(size), material, density); },
           py::arg("pose") = PxTransform(PxIdentity),
           py::arg("size") = make_array<PxReal>({1, 1, 1}), py::arg("material") = nullptr,
           py::arg("density") = 1000)
      .def("add_capsule_shape", &ActorBuilder::addCapsuleShape,
           py::arg("pose") = PxTransform(PxIdentity), py::arg("radius") = 1,
           py::arg("half_length") = 1, py::arg("material") = nullptr, py::arg("density") = 1)
      .def("add_sphere_shape", &ActorBuilder::addSphereShape,
           py::arg("pose") = PxTransform(PxIdentity), py::arg("radius") = 1,
           py::arg("material") = nullptr, py::arg("density") = 1)

      .def("add_box_visual",
           [](ActorBuilder &a, PxTransform const &pose, py::array_t<PxReal> const &size,
              py::array_t<PxReal> color, std::string const &name) {
             a.addBoxVisual(pose, array2vec3(size), array2vec3(color), name);
           },
           py::arg("pose") = PxTransform(PxIdentity),
           py::arg("size") = make_array<PxReal>({1, 1, 1}),
           py::arg("color") = make_array<PxReal>({1, 1, 1}), py::arg("name") = "")
      .def("add_capsule_visual",
           [](ActorBuilder &a, PxTransform const &pose, PxReal radius, PxReal halfLength,
              py::array_t<PxReal> color, std::string const &name) {
             a.addCapsuleVisual(pose, radius, halfLength, array2vec3(color), name);
           },
           py::arg("pose") = PxTransform(PxIdentity), py::arg("radius") = 1,
           py::arg("half_length") = 1, py::arg("color") = make_array<PxReal>({1, 1, 1}),
           py::arg("name") = "")
      .def("add_sphere_visual",
           [](ActorBuilder &a, PxTransform const &pose, PxReal radius, py::array_t<PxReal> color,
              std::string const &name) {
             a.addSphereVisual(pose, radius, array2vec3(color), name);
           },
           py::arg("pose") = PxTransform(PxIdentity), py::arg("radius") = 1,
           py::arg("color") = make_array<PxReal>({1, 1, 1}), py::arg("name") = "")
      .def("add_visual_from_file",
           [](ActorBuilder &a, std::string const &filename, PxTransform const &pose,
              py::array_t<PxReal> scale, std::string const &name) {
             a.addVisualFromFile(filename, pose, array2vec3(scale), name);
           },
           py::arg("filename"), py::arg("pose") = PxTransform(PxIdentity),
           py::arg("scale") = make_array<PxReal>({1, 1, 1}), py::arg("name") = "")

      .def("set_collision_group", &ActorBuilder::setCollisionGroup)
      .def("add_collision_group", &ActorBuilder::addCollisionGroup)
      .def("reset_collision_group", &ActorBuilder::resetCollisionGroup)
      .def("set_mass_and_inertia",
           [](ActorBuilder &a, PxReal mass, PxTransform const &cMassPose,
              py::array_t<PxReal> inertia) {
             a.setMassAndInertia(mass, cMassPose, array2vec3(inertia));
           })
      .def("set_scene", &ActorBuilder::setScene)
      .def("build", &ActorBuilder::build, py::arg("is_kinematic") = false, py::arg("name") = "",
           py::return_value_policy::reference)
      .def("build_static", &ActorBuilder::buildStatic, py::return_value_policy::reference);

  py::class_<LinkBuilder>(m, "LinkBuilder")
      .def("get_index", &LinkBuilder::getIndex)
      .def("set_parent", &LinkBuilder::setParent)
      .def("set_name", &LinkBuilder::setName)
      .def("set_joint_name", &LinkBuilder::setJointName)
      .def("set_joint_properties",
           [](LinkBuilder &b, PxArticulationJointType::Enum jointType,
              py::array_t<PxReal> const &arr, PxTransform const &parentPose,
              PxTransform const &childPose, PxReal friction, PxReal damping) {
             std::vector<std::array<PxReal, 2>> limits;
             // TODO: finish  this
           });

  py::class_<ArticulationBuilder>(m, "ArticulationBuilder")
      .def("set_scene", &ArticulationBuilder::setScene)
      .def("get_scene", &ArticulationBuilder::getScene)
      .def("create_link_builder",
           [](ArticulationBuilder &b, LinkBuilder *parent) { return b.createLinkBuilder(parent); },
           py::return_value_policy::reference)
      .def("build", &ArticulationBuilder::build, py::return_value_policy::reference);

  py::class_<URDF::URDFLoader>(m, "URDFLoader")
      .def(py::init<SScene *>())
      .def_readwrite("fix_base", &URDF::URDFLoader::fixBase)
      .def_readwrite("scale", &URDF::URDFLoader::scale)
      .def_readwrite("default_density", &URDF::URDFLoader::defaultDensity)
      .def("load", &URDF::URDFLoader::load, py::return_value_policy::reference);
}
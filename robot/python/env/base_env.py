import sapyen
from .physx_utils import transform2mat, mat2transform
import transforms3d
import numpy as np
import warnings
import os

RGBD_CAMERA_THRESHOLD = 10
CAMERA_TO_LINK = np.zeros([4, 4])
CAMERA_TO_LINK[[0, 1, 2, 3], [2, 0, 1, 3]] = [1, -1, -1, 1]


class BaseEnv:
    def __init__(self, on_screen_rendering: bool = True):
        """
        Base class of a environment of
        :param on_screen_rendering: Whether to use rendering visualization or not
        """
        # Rendering
        self.renderer = sapyen.OptifuserRenderer()
        self.renderer.set_ambient_light([.4, .4, .4])
        self.renderer.set_shadow_light([1, -1, -1], [.5, .5, .5])
        self.renderer.add_point_light([2, 2, 2], [1, 1, 1])
        self.renderer.add_point_light([2, -2, 2], [1, 1, 1])
        self.renderer.add_point_light([-2, 0, 2], [1, 1, 1])
        self.renderer.cam.set_position([0.5, -2, 2])
        self.renderer.cam.rotate_yaw_pitch(0.5, -0.5)

        # Use rendering step with render if visualization is enable
        if on_screen_rendering:
            self.step = self.__step
        else:
            self.step = lambda: self.sim.step()

        # Simulation
        self.sim = sapyen.Simulation()
        self.sim.set_renderer(self.renderer)
        self.sim.add_ground(0, material=None)
        self.simulation_hz = 200
        self.sim.set_time_step(1 / self.simulation_hz)

        # Articulation loader for both robot and object, articulation builder for simple object without joint
        self.loader = self.sim.create_urdf_loader()
        self.builder = self.sim.create_actor_builder()

        # Camera
        self.camera_frame_id = []
        self.camera_pose = []
        self.camera_name_list = []
        self.cam_list = []
        self.mount_actor_list = []
        self.mapping_list = []
        self.depth_lambda_list = []
        self.__init_camera_cache()

    def __step(self):
        """
        Step function when on screen visualization is enabled
        """
        self.sim.step()
        self.sim.update_renderer()
        self.renderer.render()

    def __init_camera_cache(self):
        """
        Init camera mapping for camera define in urdf file, like camera on robot's head.
        Camera added by user after the class instantiate will not be handle by this function
        :return:
        """
        num = self.renderer.get_camera_count()
        for i in range(num):
            camera = self.renderer.get_camera(i)
            height, width = camera.get_height(), camera.get_width()
            self.mount_actor_list.append(None)
            self.camera_name_list.append(camera.get_name())
            self.cam_list.append(camera)
            self.__build_camera_mapping(height, width, camera.get_camera_matrix())
            self.depth_lambda_list.append(
                lambda depth: 1 / (depth * (1 / camera.far - 1 / camera.near) + 1 / camera.near))

    def add_camera(self, name: str, camera_pose_mat: np.ndarray, width: int, height: int, fov=1.1, near=0.01, far=100):
        """
        Add custom mounted camera to the scene. These camera have same property as the urdf-defined camera
        :param name: Name of the camera, used for get camera name and may be used for namespace of topic if ROS enabled
        :param camera_pose_mat: (4, 4) transformation matrix to define the pose of camera. Camera point forward along
            positive x-axis, the y-axis and z-axis accounts for the width and height of the image captured by camera
        :param width: The width of the camera, e.g. 1920 in the (1920, 1080) hd image
        :param height: The height of the camera, e.g. 1080 in the (1920, 1080) hd image
        :param fov: Field of view angle in arc. Note the fov is the full angle of view, not half angle. Currently,
            we do not support differernt fov for x and y axis and thus we can only render square pixel
        :param near: Minimum distance camera can observe, it will influence all texture channel
        :param far: Maximum distance camera can observe, it will influence all texture channel
        """
        actor = self.builder.build(False, True, "{}".format(name), True)
        self.mount_actor_list.append(actor)
        self.camera_name_list.append(name)

        pose = sapyen.Pose(camera_pose_mat[:3, 3])
        pose.set_q(transforms3d.quaternions.mat2quat(camera_pose_mat[:3, :3]))
        self.sim.add_mounted_camera(name, actor, sapyen.Pose([0, 0, 0], [1, 0, 0, 0]), width, height, fov, fov,
                                    near, far)
        actor.set_global_pose(pose)

        camera = self.renderer.get_camera(len(self.cam_list))
        self.cam_list.append(camera)
        self.__build_camera_mapping(height, width, camera.get_camera_matrix())
        self.depth_lambda_list.append(lambda depth: 1 / (depth * (1 / far - 1 / near) + 1 / near))
        self.camera_frame_id.append("/base_link")
        self.camera_pose.append((camera_pose_mat @ CAMERA_TO_LINK).astype(np.float32))

    def __build_camera_mapping(self, height: int, width: int, camera_matrix: np.ndarray):
        """
        Build camera mapping matrix which maps depth to xyz point cloud
        :param height:
        :param width:
        :param camera_matrix:
        :return:
        """
        x = np.linspace(0.5, width - 0.5, width)
        y = np.linspace(0.5, height - 0.5, height)
        x, y = np.meshgrid(x, y)
        cor = np.stack([x.flatten(), y.flatten(), np.ones([x.size])], axis=0)
        mapping = np.linalg.inv(camera_matrix[:3, :3]) @ cor
        self.mapping_list.append(np.reshape(mapping.T, [height, width, 3]).astype(np.float32))

    @property
    def mounted_camera_names(self):
        """
        Names of all existing camera in order
        """
        return self.camera_name_list.copy()

    def camera_name2id(self, name: str) -> int:
        """
        Get camera id by name
        :param name: camera name
        :return: camera id or -1 if not found
        """
        if name in self.camera_name_list:
            return self.camera_name_list.index(name)
        else:
            warnings.warn("Camera name {} not found, valid camera names: {}".format(name, self.camera_name_list))
            return -1

    def render_point_cloud(self, cam_id: int, xyz: bool = True, rgba: bool = True, normal: bool = True,
                           segmentation: bool = True) -> np.ndarray:
        """
        Render all the thins expect depth map for the channel enabled
        :param cam_id: Camera id
        :param xyz: Whether to render xyz point cloud
        :param rgba: Whether to render rgba
        :param normal: Whether to render normal
        :param segmentation: Whether to render segmentation mask
        :return: array of all the enabled channels with shape (width, height, channels)
        """
        assert (xyz or rgba or normal or segmentation), "You can not call rendering function with nothing to output"
        camera = self.cam_list[cam_id]
        camera.take_picture()
        result = []

        if xyz:
            depth = self.depth_lambda_list[cam_id](camera.get_depth())[:, :, np.newaxis].astype(np.float32)
            result.append(self.mapping_list[cam_id] * depth)

        if rgba:
            color = camera.get_color_rgba()
            result.append(color)

        if normal:
            normal = camera.get_normal_rgba()
            result.append(normal[:, :, :3])

        if segmentation:
            seg = camera.get_segmentation()
            result.append(seg[:, :, np.newaxis])

        return np.concatenate(result, axis=2)


class SapienSingleObjectEnv(BaseEnv):
    def __init__(self, dataset_dir: str, data_id: int, on_screening_rendering: bool = True):
        """
        Sapien environment with single sapien object
        :param dataset_dir: Path of dataset directory
        :param data_id: Data ID of the sapien object
        """
        super(SapienSingleObjectEnv, self).__init__(on_screening_rendering)
        part_dir = os.path.join(dataset_dir, str(data_id))
        urdf = os.path.join(part_dir, "mobility.urdf")

        # By default, objects except robot will not balance passive force automatically, e.g. gravity
        self.loader.fix_loaded_object = True
        self.loader.balance_passive_force = False
        self.__urdf_file = urdf
        self.object = self.loader.load(urdf)
        self.object.set_root_pose([0, 0, 0], [1, 0, 0, 0])

        # Get the mapping for link_name, link_id, semantic_name
        self.__build_object_semantic_mapping(part_dir=part_dir)

    def __repr__(self):
        return f"Sapien Single Object Environment with object loaded from {self.__urdf_file}"

    @property
    def object_joint_names(self):
        return self.object.get_joint_names()

    @property
    def object_link_names(self):
        return self.object.get_link_names()

    @property
    def object_links(self):
        return self.object.get_links()

    @property
    def object_link_segmentation_ids(self):
        return self.object.get_link_ids()

    @property
    def object_link_motions(self):
        return self.__object_link_motion

    @property
    def object_link_semantics(self):
        return self.__object_link_semantics

    def object_name2link(self, name: str) -> sapyen.PxRigidBody:
        return self.__object_name2link[name]

    def object_segmentation_id2semantics(self, segmentation_id: int) -> str:
        return self.__object_segmentation_id2semantics[segmentation_id]

    def __build_object_semantic_mapping(self, part_dir: str):
        """
        Build internally stored cache for objects
        :param part_dir:
        :return:
        """
        semantics = os.path.join(part_dir, 'semantics.txt')
        link2semantics = {}
        link2motion = {}
        id2link_name = dict(zip(self.object.get_link_ids(), self.object.get_link_names()))
        with open(semantics, 'r') as f:
            for line in f:
                if line.strip():
                    link, motion, semantics = line.split()
                    link2semantics[link] = semantics
                    link2motion[link] = motion

        link_names = self.object.get_link_names()
        links = self.object.get_links()
        self.__object_link_semantics = [link2semantics[link] for link in link_names]
        self.__object_link_motion = [link2motion[link] for link in link_names]
        self.__object_name2link = {link_names[i]: links[i] for i in range(len(link_names))}
        self.__object_segmentation_id2semantics = {i: link2semantics[id2link_name[i]] for i in
                                                   self.object_link_segmentation_ids}

    def apply_general_force_torque(self, link_index: int, force_array: np.ndarray):
        """
        Apply force and torque to a object link
        :param link_index: Index of object link
        :param force_array: 6d array for force and torque, xyz convention
        """
        assert len(force_array) == 6
        link = self.object_links[link_index]
        link.add_force(force_array)
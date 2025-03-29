import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import List

import numpy as np
from evo.core.trajectory import xyz_quat_wxyz_to_se3_poses


def get_transformation_matrix(T_AB_t_xyz_q_xyzw):
    assert len(T_AB_t_xyz_q_xyzw) == 7, f"only got {len(T_AB_t_xyz_q_xyzw)} params"
    t_xyz = T_AB_t_xyz_q_xyzw[:3]
    q_xyzw = T_AB_t_xyz_q_xyzw[3:]
    q_wxyz = [q_xyzw[-1]] + q_xyzw[:-1]
    return xyz_quat_wxyz_to_se3_poses([t_xyz], [q_wxyz])[0]

#  T_base_lidar_t_xyz_q_xyzw: [0.0, 0.0, 0.124, 0.0, 0.0, 1.0, 0.0]
t_xyz = [0.0, 0.0, 0.124]
q_xyzw = [0.0, 0.0, 1.0, 0.0]
q_wxyz = [q_xyzw[-1]] + q_xyzw[:-1]
print(xyz_quat_wxyz_to_se3_poses([t_xyz], [q_wxyz]))